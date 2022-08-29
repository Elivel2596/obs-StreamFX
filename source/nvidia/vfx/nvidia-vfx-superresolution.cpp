// Copyright (c) 2020 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "nvidia-vfx-superresolution.hpp"
#include "obs/gs/gs-helper.hpp"
#include "util/util-logging.hpp"
#include "util/utility.hpp"

#include "warning-disable.hpp"
#include <cmath>
#include <utility>
#include <vector>
#include "warning-enable.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::vfx::superresolution::superresolution> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

static std::vector<float> supported_scale_factors{4. / 3., 1.5, 2., 3., 4.};

static float find_closest_scale_factor(float factor)
{
	std::pair<float, float> minimal = {0.f, std::numeric_limits<float>::max()};
	for (float delta : supported_scale_factors) {
		float value = abs(delta - factor);
		if (minimal.second > value) {
			minimal.first  = delta;
			minimal.second = value;
		}
	}

	return minimal.first;
}

static size_t find_closest_scale_factor_index(float factor)
{
	std::pair<size_t, float> minimal = {0, std::numeric_limits<float>::max()};
	for (size_t idx = 0; idx < supported_scale_factors.size(); idx++) {
		float delta = supported_scale_factors[idx];
		float value = abs(delta - factor);
		if (minimal.second > value) {
			minimal.first  = idx;
			minimal.second = value;
		}
	}

	return minimal.first;
}

streamfx::nvidia::vfx::superresolution::~superresolution()
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	// Clean up any CUDA resources in use.
	_input.reset();
	_convert_to_fp32.reset();
	_source.reset();
	_destination.reset();
	_convert_to_u8.reset();
	_output.reset();
	_tmp.reset();
}

streamfx::nvidia::vfx::superresolution::superresolution()
	: effect(EFFECT_SUPERRESOLUTION), _dirty(true), _input(), _convert_to_fp32(), _source(), _destination(),
	  _convert_to_u8(), _output(), _tmp(), _strength(1.), _scale(1.5), _cache_input_size(), _cache_output_size(),
	  _cache_scale()
{
	// Enter Graphics and CUDA context.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	// Set the strength, scale and buffers.
	set_strength(_strength);
	set_scale(_scale);
	resize(160, 90);

	// Load the effect.
	load();
}

void streamfx::nvidia::vfx::superresolution::set_strength(float strength)
{
	strength = (strength >= .5f) ? 1.f : 0.f;
	std::swap(_strength, strength);

	// If anything was changed, flag the effect as dirty.
	if (!::streamfx::util::math::is_close<float>(_strength, strength, 0.01f))
		_dirty = true;

	// Update Effect
	uint32_t value = (_strength >= .5f) ? 1u : 0u;
	auto     gctx  = ::streamfx::obs::gs::context();
	auto     cctx  = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();
	if (auto res = set(::streamfx::nvidia::vfx::PARAMETER_STRENGTH, value);
		res != ::streamfx::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to set '%s' to %lu.", ::streamfx::nvidia::vfx::PARAMETER_STRENGTH, value);
	};
}

float streamfx::nvidia::vfx::superresolution::strength()
{
	return _strength;
}

void streamfx::nvidia::vfx::superresolution::set_scale(float scale)
{
	// Limit to acceptable range.
	scale = std::clamp<float>(scale, 1., 4.);

	// Match to nearest scale.
	float factor = static_cast<float>(find_closest_scale_factor(scale));

	// If anything was changed, flag the effect as dirty.
	if (!::streamfx::util::math::is_close<float>(_scale, factor, 0.01f))
		_dirty = true;

	// Save new scale factor.
	_scale = factor;
}

float streamfx::nvidia::vfx::superresolution::scale()
{
	return _scale;
}

void streamfx::nvidia::vfx::superresolution::size(std::pair<uint32_t, uint32_t> const& size,
												  std::pair<uint32_t, uint32_t>&       input_size,
												  std::pair<uint32_t, uint32_t>&       output_size)
{
	// Check if the size has actually changed at all.
	if ((input_size.first == _cache_input_size.first) && (input_size.second == _cache_input_size.second)
		&& (_scale == _cache_scale)) {
		input_size  = _cache_input_size;
		output_size = _cache_output_size;
		_scale      = _cache_scale;
		return;
	}

	// Define lower and upper boundaries for resolution.
	constexpr uint32_t min_width  = 160;
	constexpr uint32_t min_height = 90;
	uint32_t           max_width  = 0;
	uint32_t           max_height = 0;
	if (_scale > 3.0) {
		max_width  = 960;
		max_height = 540;
	} else if (_scale > 2.0) {
		max_width  = 1280;
		max_height = 720;
	} else {
		max_width  = 1920;
		max_height = 1080;
	}

	// Restore Input Size
	input_size.first  = size.first;
	input_size.second = size.second;

	// Calculate Input Size
	if (input_size.first > input_size.second) {
		// Dominant Width
		double ar         = static_cast<double>(input_size.second) / static_cast<double>(input_size.first);
		input_size.first  = std::clamp<uint32_t>(input_size.first, min_width, max_width);
		input_size.second = std::clamp<uint32_t>(
			static_cast<uint32_t>(std::lround(static_cast<double>(input_size.first) * ar)), min_height, max_height);
	} else {
		// Dominant Height
		double ar         = static_cast<double>(input_size.first) / static_cast<double>(input_size.second);
		input_size.second = std::clamp<uint32_t>(input_size.second, min_height, max_height);
		input_size.first  = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::lround(static_cast<double>(input_size.second) * ar)), min_width, max_width);
	}

	// Calculate Output Size.
	output_size.first  = static_cast<uint32_t>(std::lround(static_cast<float>(input_size.first) * _scale));
	output_size.second = static_cast<uint32_t>(std::lround(static_cast<float>(input_size.second) * _scale));

	// Verify that this is a valid scale factor.
	float width_mul  = (static_cast<float>(output_size.first) / static_cast<float>(input_size.first));
	float height_mul = (static_cast<float>(output_size.second) / static_cast<float>(input_size.second));
	if (!::streamfx::util::math::is_close<float>(width_mul, _scale, 0.00001)
		|| !::streamfx::util::math::is_close<float>(height_mul, _scale, 0.00001)) {
		size_t scale_idx = find_closest_scale_factor_index(_scale);
		if (scale_idx < supported_scale_factors.size()) {
			_scale = supported_scale_factors[scale_idx + 1];
			this->size(size, input_size, output_size);
		}
	}

	// Update last stored values.
	_cache_input_size  = input_size;
	_cache_output_size = output_size;
	_cache_scale       = _scale;
}

std::shared_ptr<::streamfx::obs::gs::texture>
	streamfx::nvidia::vfx::superresolution::process(std::shared_ptr<::streamfx::obs::gs::texture> in)
{
	// Enter Graphics and CUDA context.
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = _nvcuda->get_context()->enter();

#ifdef ENABLE_PROFILING
	::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_magenta, "NvVFX Super-Resolution"};
#endif

	// Resize if the size or scale was changed.
	resize(in->get_width(), in->get_height());

	// Reload effect if dirty.
	if (_dirty) {
		load();
	}

	{ // Copy parameter to input.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy In -> Input"};
#endif
		gs_copy_texture(_input->get_texture()->get_object(), in->get_object());
	}

	{ // Convert Input to Source format
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_convert,
													"Convert Input -> Source"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_input->get_image(), _convert_to_fp32->get_image(), 1.f,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer processing result to output due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	{ // Copy input to source.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy, "Copy Input -> Source"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_convert_to_fp32->get_image(), _source->get_image(), 1.f,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer input to processing source due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	{ // Process source to destination.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_cache, "Process"};
#endif
		if (auto res = run(); res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to process due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Run failed.");
		}
	}

	{ // Convert Destination to Output format
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_convert,
													"Convert Destination -> Output"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_destination->get_image(), _convert_to_u8->get_image(), 1.f,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer processing result to output due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	{ // Copy destination to output.
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_copy,
													"Copy Destination -> Output"};
#endif
		if (auto res = _nvcvi->NvCVImage_Transfer(_convert_to_u8->get_image(), _output->get_image(), 1.,
												  _nvcuda->get_stream()->get(), _tmp->get_image());
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to transfer processing result to output due to error: %s",
						_nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("Transfer failed.");
		}
	}

	// Return output.
	return _output->get_texture();
}

void streamfx::nvidia::vfx::superresolution::resize(uint32_t width, uint32_t height)
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	_cache_input_size = {width, height};
	this->size(_cache_input_size, _cache_input_size, _cache_output_size);

	if (!_tmp) {
		_tmp = std::make_shared<::streamfx::nvidia::cv::image>(
			_cache_output_size.first, _cache_output_size.second, ::streamfx::nvidia::cv::pixel_format::RGBA,
			::streamfx::nvidia::cv::component_type::UINT8, ::streamfx::nvidia::cv::component_layout::PLANAR,
			::streamfx::nvidia::cv::memory_location::GPU, 1);
	}

	if (!_input || (_input->get_image()->width != _cache_input_size.first)
		|| (_input->get_image()->height != _cache_input_size.second)) {
		if (_input) {
			_input->resize(_cache_input_size.first, _cache_input_size.second);
		} else {
			_input = std::make_shared<::streamfx::nvidia::cv::texture>(_cache_input_size.first,
																	   _cache_input_size.second, GS_RGBA_UNORM);
		}
	}

	if (!_convert_to_fp32 || (_convert_to_fp32->get_image()->width != _cache_input_size.first)
		|| (_convert_to_fp32->get_image()->height != _cache_input_size.second)) {
		if (_convert_to_fp32) {
			_convert_to_fp32->resize(_cache_input_size.first, _cache_input_size.second);
		} else {
			_convert_to_fp32 = std::make_shared<::streamfx::nvidia::cv::image>(
				_cache_input_size.first, _cache_input_size.second, ::streamfx::nvidia::cv::pixel_format::RGBA,
				::streamfx::nvidia::cv::component_type::FP32, ::streamfx::nvidia::cv::component_layout::PLANAR,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}
	}

	if (!_source || (_source->get_image()->width != _cache_input_size.first)
		|| (_source->get_image()->height != _cache_input_size.second)) {
		if (_source) {
			_source->resize(_cache_input_size.first, _cache_input_size.second);
		} else {
			_source = std::make_shared<::streamfx::nvidia::cv::image>(
				_cache_input_size.first, _cache_input_size.second, ::streamfx::nvidia::cv::pixel_format::BGR,
				::streamfx::nvidia::cv::component_type::FP32, ::streamfx::nvidia::cv::component_layout::PLANAR,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (auto res = set(::streamfx::nvidia::vfx::PARAMETER_INPUT_IMAGE_0, _source);
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set input image due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetImage failed.");
		}

		_dirty = true;
	}

	if (!_destination || (_destination->get_image()->width != _cache_output_size.first)
		|| (_destination->get_image()->height != _cache_output_size.second)) {
		if (_destination) {
			_destination->resize(_cache_output_size.first, _cache_output_size.second);
		} else {
			_destination = std::make_shared<::streamfx::nvidia::cv::image>(
				_cache_output_size.first, _cache_output_size.second, ::streamfx::nvidia::cv::pixel_format::BGR,
				::streamfx::nvidia::cv::component_type::FP32, ::streamfx::nvidia::cv::component_layout::PLANAR,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}

		if (auto res = set(::streamfx::nvidia::vfx::PARAMETER_OUTPUT_IMAGE_0, _destination);
			res != ::streamfx::nvidia::cv::result::SUCCESS) {
			D_LOG_ERROR("Failed to set output image due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
			throw std::runtime_error("SetImage failed.");
		}

		_dirty = true;
	}

	if (!_convert_to_u8 || (_convert_to_u8->get_image()->width != _cache_output_size.first)
		|| (_convert_to_u8->get_image()->height != _cache_output_size.second)) {
		if (_convert_to_u8) {
			_convert_to_u8->resize(_cache_output_size.first, _cache_output_size.second);
		} else {
			_convert_to_u8 = std::make_shared<::streamfx::nvidia::cv::image>(
				_cache_output_size.first, _cache_output_size.second, ::streamfx::nvidia::cv::pixel_format::RGBA,
				::streamfx::nvidia::cv::component_type::UINT8, ::streamfx::nvidia::cv::component_layout::INTERLEAVED,
				::streamfx::nvidia::cv::memory_location::GPU, 1);
		}
	}

	if (!_output || (_output->get_image()->width != _cache_output_size.first)
		|| (_output->get_image()->height != _cache_output_size.second)) {
		if (_output) {
			_output->resize(_cache_output_size.first, _cache_output_size.second);
		} else {
			_output = std::make_shared<::streamfx::nvidia::cv::texture>(_cache_output_size.first,
																		_cache_output_size.second, GS_RGBA_UNORM);
		}
	}
}

void streamfx::nvidia::vfx::superresolution::load()
{
	auto gctx = ::streamfx::obs::gs::context();
	auto cctx = ::streamfx::nvidia::cuda::obs::get()->get_context()->enter();

	if (auto res = effect::load(); res != ::streamfx::nvidia::cv::result::SUCCESS) {
		D_LOG_ERROR("Failed to initialize effect due to error: %s", _nvcvi->NvCV_GetErrorStringFromCode(res));
		throw std::runtime_error("Load failed.");
	}

	_dirty = false;
}
