#ifdef Q_OS_WINDOWS

#include "d3d11presenter.h"

#include "qmlmainwindow.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>

namespace
{
	const wchar_t child_class[] = L"ChiakiNGD3D11Direct";

	LRESULT CALLBACK child_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}

	void av_lock_cb(void *lock_ctx)
	{
		EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(lock_ctx));
	}

	void av_unlock_cb(void *lock_ctx)
	{
		LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(lock_ctx));
	}
}

D3D11Presenter::D3D11Presenter(QObject *parent)
	: QObject(parent)
{
}

void D3D11Presenter::cleanupRenderResources()
{
	if(av_hw_device_ctx)
		av_buffer_unref(&av_hw_device_ctx);
	if(vp_output_view)
		vp_output_view->Release();
	if(video_processor)
		video_processor->Release();
	if(vp_enumerator)
		vp_enumerator->Release();
	if(rgba_texture)
		rgba_texture->Release();
	if(nv12_texture)
		nv12_texture->Release();
	if(swapchain)
		swapchain->Release();
	if(video_context)
		video_context->Release();
	if(video_device)
		video_device->Release();
	if(context)
		context->Release();
	if(device)
		device->Release();
	vp_output_view = nullptr;
	video_processor = nullptr;
	vp_enumerator = nullptr;
	rgba_texture = nullptr;
	nv12_texture = nullptr;
	swapchain = nullptr;
	video_context = nullptr;
	video_device = nullptr;
	context = nullptr;
	device = nullptr;
	output_width = 0;
	output_height = 0;
}

D3D11Presenter::~D3D11Presenter()
{
	stop();
	cleanupRenderResources();

	if(av_lock)
	{
		DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(av_lock));
		delete reinterpret_cast<CRITICAL_SECTION *>(av_lock);
	}
}

bool D3D11Presenter::initialize(QWindow *parent_window, int frame_width, int frame_height)
{
	if(!parent_window || frame_width <= 0 || frame_height <= 0)
		return false;

	// Fresh resources for each session (device refs may be held by the
	// previous decoder, so full teardown keeps repeated sessions leak-free).
	stop();
	cleanupRenderResources();

	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL feature_levels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
			feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
			&device, nullptr, &context);
	if(FAILED(hr))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] D3D11CreateDevice failed" << QString::number(hr, 16);
		return false;
	}

	ID3D11Multithread *multithread = nullptr;
	if(SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&multithread))))
	{
		multithread->SetMultithreadProtected(TRUE);
		multithread->Release();
	}

	if(FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&video_device))) ||
			FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&video_context))))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] ID3D11VideoDevice/VideoContext unavailable";
		return false;
	}

	// Wrap the device for FFmpeg so d3d11va decodes into surfaces on this device.
	av_lock = new CRITICAL_SECTION;
	InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(av_lock));
	av_hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if(!av_hw_device_ctx)
	{
		qCWarning(chiakiGui) << "[d3d11-direct] av_hwdevice_ctx_alloc failed";
		return false;
	}
	auto *av_ctx = reinterpret_cast<AVD3D11VADeviceContext *>(av_hw_device_ctx->data);
	av_ctx->device = device;
	av_ctx->device_context = context;
	av_ctx->video_device = video_device;
	av_ctx->video_context = video_context;
	av_ctx->lock = av_lock_cb;
	av_ctx->unlock = av_unlock_cb;
	av_ctx->lock_ctx = av_lock;
	device->AddRef();
	context->AddRef();
	video_device->AddRef();
	video_context->AddRef();
	if(av_hwdevice_ctx_init(av_hw_device_ctx) < 0)
	{
		qCWarning(chiakiGui) << "[d3d11-direct] av_hwdevice_ctx_init failed";
		av_buffer_unref(&av_hw_device_ctx);
		return false;
	}

	stream_width = frame_width;
	stream_height = frame_height;

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
	content_desc.InputWidth = stream_width;
	content_desc.InputHeight = stream_height;
	content_desc.OutputWidth = stream_width;
	content_desc.OutputHeight = stream_height;
	content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if(FAILED(video_device->CreateVideoProcessorEnumerator(&content_desc, &vp_enumerator)) ||
			FAILED(video_device->CreateVideoProcessor(vp_enumerator, 0, &video_processor)))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] VideoProcessor creation failed";
		return false;
	}

	BOOL enable = TRUE;
	video_context->VideoProcessorSetStreamEnable(video_processor, 0, enable);

	D3D11_VIDEO_PROCESSOR_COLOR_SPACE stream_color = {};
	stream_color.YCbCr_Matrix = 1; // BT.709
	stream_color.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
	video_context->VideoProcessorSetStreamColorSpace(video_processor, 0, &stream_color);

	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
	output_color.Usage = 0;
	output_color.RGB_Range = 0; // full range
	video_context->VideoProcessorSetOutputColorSpace(video_processor, &output_color);

	RECT source = { 0, 0, static_cast<LONG>(stream_width), static_cast<LONG>(stream_height) };
	video_context->VideoProcessorSetStreamSourceRect(video_processor, 0, TRUE, &source);

	// Child window covering the QML window's client area.
	HWND parent_hwnd = reinterpret_cast<HWND>(parent_window->winId());
	RECT client_rect;
	if(!GetClientRect(parent_hwnd, &client_rect))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] GetClientRect failed";
		return false;
	}
	POINT origin = { client_rect.left, client_rect.top };
	MapWindowPoints(parent_hwnd, nullptr, &origin, 1);
	int win_w = client_rect.right - client_rect.left;
	int win_h = client_rect.bottom - client_rect.top;

	static bool class_registered = false;
	if(!class_registered)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = child_wndproc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = child_class;
		if(!RegisterClassExW(&wc))
			qCWarning(chiakiGui) << "[d3d11-direct] RegisterClassExW failed (may already be registered)";
		class_registered = true;
	}

	HWND hwnd = CreateWindowExW(0, child_class, L"",
			WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
			origin.x, origin.y, win_w, win_h,
			parent_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
	if(!hwnd)
	{
		qCWarning(chiakiGui) << "[d3d11-direct] CreateWindowExW failed";
		return false;
	}
	window_handle = hwnd;
	parent_qwindow = parent_window;
	last_x = origin.x;
	last_y = origin.y;
	last_w = win_w;
	last_h = win_h;

	IDXGIDevice *dxgi_device = nullptr;
	IDXGIAdapter *adapter = nullptr;
	IDXGIFactory2 *factory = nullptr;
	bool swapchain_created = false;
	if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) &&
			SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
			SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory))))
	{
		DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
		sc_desc.Width = win_w;
		sc_desc.Height = win_h;
		sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sc_desc.SampleDesc.Count = 1;
		sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sc_desc.BufferCount = 2;
		sc_desc.Scaling = DXGI_SCALING_STRETCH;
		sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		hr = factory->CreateSwapChainForHwnd(device, hwnd, &sc_desc, nullptr, nullptr, &swapchain);
		swapchain_created = SUCCEEDED(hr);
		factory->Release();
		adapter->Release();
		dxgi_device->Release();
	}
	if(!swapchain_created)
	{
		qCWarning(chiakiGui) << "[d3d11-direct] CreateSwapChainForHwnd failed" << QString::number(hr, 16);
		return false;
	}

	if(!ensureOutputSize(win_w, win_h))
		return false;

	initialized = true;
	qCInfo(chiakiGui) << "[d3d11-direct] active:" << stream_width << "x" << stream_height
		<< "->" << win_w << "x" << win_h
		<< "(d3d11va -> VideoProcessor -> flip swapchain, no libplacebo path)";
	return true;
}

void D3D11Presenter::stop()
{
	initialized = false;
	if(window_handle)
	{
		DestroyWindow(reinterpret_cast<HWND>(window_handle));
		window_handle = nullptr;
		parent_qwindow = nullptr;
	}
}

bool D3D11Presenter::ensureOutputSize(int width, int height)
{
	if(width <= 0 || height <= 0)
		return false;
	if(width == output_width && height == output_height && rgba_texture && vp_output_view)
		return true;

	if(rgba_texture)
	{
		rgba_texture->Release();
		rgba_texture = nullptr;
	}
	D3D11_TEXTURE2D_DESC tex_desc = {};
	tex_desc.Width = width;
	tex_desc.Height = height;
	tex_desc.MipLevels = 1;
	tex_desc.ArraySize = 1;
	tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if(FAILED(device->CreateTexture2D(&tex_desc, nullptr, &rgba_texture)))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] RGBA texture creation failed";
		return false;
	}

	if(vp_output_view)
	{
		vp_output_view->Release();
		vp_output_view = nullptr;
	}
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc = {};
	out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if(FAILED(video_device->CreateVideoProcessorOutputView(rgba_texture, vp_enumerator, &out_desc, &vp_output_view)))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] VideoProcessorOutputView creation failed";
		return false;
	}

	// Letterbox rect preserving the stream aspect ratio.
	LONG dst_w = width;
	LONG dst_h = static_cast<LONG>(static_cast<double>(width) * stream_height / stream_width);
	if(dst_h > height)
	{
		dst_h = height;
		dst_w = static_cast<LONG>(static_cast<double>(height) * stream_width / stream_height);
	}
	RECT dest = { (width - dst_w) / 2, (height - dst_h) / 2, (width - dst_w) / 2 + dst_w, (height - dst_h) / 2 + dst_h };
	video_context->VideoProcessorSetStreamDestinationRect(video_processor, 0, TRUE, &dest);

	HRESULT hr = swapchain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
	if(FAILED(hr))
	{
		qCWarning(chiakiGui) << "[d3d11-direct] ResizeBuffers failed" << QString::number(hr, 16);
		return false;
	}

	output_width = width;
	output_height = height;
	return true;
}

bool D3D11Presenter::updateGeometry()
{
	if(!parent_qwindow || !window_handle)
		return false;
	HWND parent_hwnd = reinterpret_cast<HWND>(parent_qwindow->winId());
	RECT client_rect;
	if(!GetClientRect(parent_hwnd, &client_rect))
		return false;
	int w = client_rect.right - client_rect.left;
	int h = client_rect.bottom - client_rect.top;
	POINT origin = { client_rect.left, client_rect.top };
	MapWindowPoints(parent_hwnd, nullptr, &origin, 1);

	if(w != last_w || h != last_h || origin.x != last_x || origin.y != last_y)
	{
		if(!SetWindowPos(reinterpret_cast<HWND>(window_handle), nullptr,
					origin.x, origin.y, w, h, SWP_NOACTIVATE | SWP_NOZORDER))
			return false;
		if(!ensureOutputSize(w, h))
			return false;
		last_x = origin.x;
		last_y = origin.y;
		last_w = w;
		last_h = h;
	}
	return true;
}

bool D3D11Presenter::presentD3D11Frame(AVFrame *frame)
{
	if(!initialized || !frame || frame->format != AV_PIX_FMT_D3D11)
		return false;

	ID3D11Texture2D *decoder_texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
	UINT array_slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
	if(!decoder_texture)
		return false;

	if(!updateGeometry())
		return false;
	return presentSurface(decoder_texture, array_slice);
}

bool D3D11Presenter::presentSurface(ID3D11Texture2D *decoder_texture, UINT array_slice)
{
	// Copy the decoder surface into our own NV12 texture so the video processor
	// input view does not depend on driver quirks around D3D11_BIND_DECODER
	// surfaces. GPU-to-GPU copy, no CPU involvement.
	D3D11_TEXTURE2D_DESC want;
	decoder_texture->GetDesc(&want);
	want.BindFlags = D3D11_BIND_RENDER_TARGET;
	want.MiscFlags = 0;
	want.CPUAccessFlags = 0;

	bool need_create = !nv12_texture;
	if(nv12_texture)
	{
		D3D11_TEXTURE2D_DESC have;
		nv12_texture->GetDesc(&have);
		need_create = have.Width != want.Width || have.Height != want.Height ||
			have.Format != want.Format || have.ArraySize != want.ArraySize;
	}
	if(need_create)
	{
		if(nv12_texture)
			nv12_texture->Release();
		nv12_texture = nullptr;
		if(FAILED(device->CreateTexture2D(&want, nullptr, &nv12_texture)))
		{
			qCWarning(chiakiGui) << "[d3d11-direct] NV12 texture creation failed";
			return false;
		}
	}

	context->CopySubresourceRegion(nv12_texture, array_slice, 0, 0, 0, decoder_texture, array_slice, nullptr);

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc = {};
	in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	in_desc.Texture2D.MipSlice = 0;
	in_desc.Texture2D.ArraySlice = array_slice;
	ID3D11VideoProcessorInputView *input_view = nullptr;
	if(FAILED(video_device->CreateVideoProcessorInputView(nv12_texture, vp_enumerator, &in_desc, &input_view)))
	{
		static bool view_logged = false;
		logHrOnce("VideoProcessorInputView", E_FAIL, &view_logged);
		return false;
	}

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = input_view;

	RECT output_rect = { 0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height) };
	video_context->VideoProcessorSetOutputTargetRect(video_processor, TRUE, &output_rect);

	HRESULT hr = video_context->VideoProcessorBlt(video_processor, vp_output_view, 0, 1, &stream);
	input_view->Release();
	if(FAILED(hr))
	{
		static bool blt_logged = false;
		logHrOnce("VideoProcessorBlt", hr, &blt_logged);
		return false;
	}

	ID3D11Texture2D *backbuffer = nullptr;
	if(FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backbuffer))))
		return false;
	context->CopyResource(backbuffer, rgba_texture);
	backbuffer->Release();

	hr = swapchain->Present(1, 0); // vsync-locked flip present
	if(FAILED(hr))
	{
		static bool present_logged = false;
		logHrOnce("Present", hr, &present_logged);
		return false;
	}
	return true;
}

void D3D11Presenter::logHrOnce(const char *what, long hr, bool *logged)
{
	if(*logged)
		return;
	*logged = true;
	qCWarning(chiakiGui) << "[d3d11-direct]" << what << "failed" << QString::number(static_cast<qulonglong>(hr), 16)
		<< "- frames will fall back to the regular render path";
}

#endif // Q_OS_WINDOWS
