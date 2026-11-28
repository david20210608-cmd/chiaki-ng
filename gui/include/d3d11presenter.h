#pragma once

#include <QtGlobal>

#ifdef Q_OS_WINDOWS

#include <QObject>
#include <QWindow>

struct AVBufferRef;
struct AVFrame;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11VideoContext;
struct ID3D11VideoDevice;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorOutputView;
struct IDXGISwapChain1;

// Direct D3D11 presentation path for hardware-decoded frames:
// decoder surfaces stay on the GPU and reach the display through the
// fixed-function video processor, bypassing the libplacebo render path
// (no GPU->CPU->GPU roundtrip, no full-screen shader passes).
class D3D11Presenter : public QObject
{
	public:
		explicit D3D11Presenter(QObject *parent = nullptr);
		~D3D11Presenter() override;

		bool initialize(QWindow *parent_window, int stream_width, int stream_height);
		AVBufferRef *avHwDeviceCtx() const { return av_hw_device_ctx; }
		bool active() const { return initialized; }
		void stop();
		bool presentD3D11Frame(AVFrame *frame);

	private:
		bool ensureOutputSize(int width, int height);
		bool updateGeometry();
		bool presentSurface(ID3D11Texture2D *texture, unsigned int array_slice);
		void logHrOnce(const char *what, long hr, bool *logged);
		void cleanupRenderResources();

		bool initialized = false;
		int stream_width = 0;
		int stream_height = 0;
		int output_width = 0;
		int output_height = 0;
		int last_x = -1;
		int last_y = -1;
		int last_w = -1;
		int last_h = -1;

		void *window_handle = nullptr; // child HWND
		QWindow *parent_qwindow = nullptr;

		ID3D11Device *device = nullptr;
		ID3D11DeviceContext *context = nullptr;
		ID3D11VideoDevice *video_device = nullptr;
		ID3D11VideoContext *video_context = nullptr;
		ID3D11Texture2D *nv12_texture = nullptr; // staging copy of decoder surfaces
		ID3D11Texture2D *rgba_texture = nullptr;
		ID3D11VideoProcessorEnumerator *vp_enumerator = nullptr;
		ID3D11VideoProcessor *video_processor = nullptr;
		ID3D11VideoProcessorOutputView *vp_output_view = nullptr;
		IDXGISwapChain1 *swapchain = nullptr;

		AVBufferRef *av_hw_device_ctx = nullptr;
		void *av_lock = nullptr; // CRITICAL_SECTION*
};

#endif // Q_OS_WINDOWS
