#include "pch.h"

#include <stdlib.h>
#include <shellapi.h>
#include <fstream>
#include <iostream>

#include "macros.h"
#include "CubeRenderer.h"
#include "DeviceResources.h"

#include "config_parser.h"
#include "directx_multi_peer_conductor.h"
#include "server_main_window.h"
#include "server_renderer.h"
#include "service/render_service.h"
#include "webrtc.h"

// Position the cube two meters in front of user for image stabilization.
#define FOCUS_POINT					-2.0f

// If clients don't send "stereo-rendering" message after this time,
// the video stream will start in non-stereo mode.
#define STEREO_FLAG_WAIT_TIME		5000

// Required app libs
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "usp10.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")


using namespace Microsoft::WRL;
using namespace Windows::Foundation::Numerics;


using namespace DX;
using namespace StreamingToolkit;
using namespace StreamingToolkitSample;

//--------------------------------------------------------------------------------------
// Global Variables
//--------------------------------------------------------------------------------------
DeviceResources*					g_deviceResources = nullptr;
CubeRenderer*						g_cubeRenderer = nullptr;

// Remote peer data
struct RemotePeerData
{
	// True if this data hasn't been processed
	bool							isNew;

	// True for stereo output, false otherwise
	bool							isStereo;

	// The look at vector used in camera transform
	DirectX::XMVECTORF32			lookAtVector;

	// The up vector used in camera transform
	DirectX::XMVECTORF32			upVector;

	// The eye vector used in camera transform
	DirectX::XMVECTORF32			eyeVector;

	// The projection matrix for left eye used in camera transform
	DirectX::XMFLOAT4X4				projectionMatrixLeft;

	// The view matrix for left eye used in camera transform
	DirectX::XMFLOAT4X4				viewMatrixLeft;

	// The projection matrix for right eye used in camera transform
	DirectX::XMFLOAT4X4				projectionMatrixRight;

	// The view matrix for right eye used in camera transform
	DirectX::XMFLOAT4X4				viewMatrixRight;

	// The timestamp used for frame synchronization in stereo mode
	int64_t							lastTimestamp;

	// The render texture which we use to render
	ComPtr<ID3D11Texture2D>			renderTexture;

	// The render target view of the render texture
	ComPtr<ID3D11RenderTargetView>	renderTargetView;

	// The depth stencil texture which we use to render
	ComPtr<ID3D11Texture2D>			depthStencilTexture;

	// The depth stencil view of the depth stencil texture
	ComPtr<ID3D11DepthStencilView>	depthStencilView;

	// Used for FPS limiter.
	ULONGLONG						tick;

	// The starting time.
	ULONGLONG						startTick;
};

std::map<int, std::shared_ptr<RemotePeerData>> g_remotePeersData;
DirectXMultiPeerConductor* g_cond;

void InitializeRenderTexture(RemotePeerData* peerData, int width, int height, bool isStereo)
{
	int texWidth = isStereo ? width << 1 : width;
	int texHeight = height;

	// Creates the render texture.
	D3D11_TEXTURE2D_DESC texDesc = { 0 };
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.Width = texWidth;
	texDesc.Height = texHeight;
	texDesc.MipLevels = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
	g_deviceResources->GetD3DDevice()->CreateTexture2D(&texDesc, nullptr, &peerData->renderTexture);

	// Creates the render target view.
	g_deviceResources->GetD3DDevice()->CreateRenderTargetView(peerData->renderTexture.Get(), nullptr, &peerData->renderTargetView);
}

void InitializeDepthStencilTexture(RemotePeerData* peerData, int width, int height, bool isStereo)
{
	int texWidth = isStereo ? width << 1 : width;
	int texHeight = height;

	// Creates the depth stencil texture.
	D3D11_TEXTURE2D_DESC texDesc = { 0 };
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	texDesc.Width = texWidth;
	texDesc.Height = texHeight;
	texDesc.MipLevels = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	g_deviceResources->GetD3DDevice()->CreateTexture2D(&texDesc, nullptr, &peerData->depthStencilTexture);

	// Creates the depth stencil view.
	D3D11_DEPTH_STENCIL_VIEW_DESC descDSV;
	descDSV.Format = texDesc.Format;
	descDSV.Flags = 0;
	descDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	descDSV.Texture2D.MipSlice = 0;
	g_deviceResources->GetD3DDevice()->CreateDepthStencilView(peerData->depthStencilTexture.Get(), &descDSV, &peerData->depthStencilView);
}

bool AppMain(BOOL stopping)
{
	auto fullServerConfig = GlobalObject<FullServerConfig>::Get();
	auto nvEncConfig = GlobalObject<NvEncConfig>::Get();

	rtc::EnsureWinsockInit();
	rtc::Win32Thread w32_thread;
	rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);

	ServerMainWindow wnd(
		fullServerConfig->webrtc_config->server_uri.c_str(),
		fullServerConfig->webrtc_config->port,
		fullServerConfig->server_config->server_config.auto_connect,
		fullServerConfig->server_config->server_config.auto_call,
		false,
		fullServerConfig->server_config->server_config.width,
		fullServerConfig->server_config->server_config.height);

	if (!fullServerConfig->server_config->server_config.system_service && !wnd.Create())
	{
		RTC_NOTREACHED();
		return -1;
	}

	// Initializes the device resources.
	g_deviceResources = new DeviceResources();
	g_deviceResources->SetWindow(wnd.handle());

	// Initializes the cube renderer.
	g_cubeRenderer = new CubeRenderer(g_deviceResources);

	// Initializes SSL.
	rtc::InitializeSSL();

	// Initializes the conductor.
	DirectXMultiPeerConductor cond(fullServerConfig, g_deviceResources->GetD3DDevice());

	// Sets main window to update UI.
	cond.SetMainWindow(&wnd);

	// Registers the handler.
	wnd.RegisterObserver(&cond);

	// Handles data channel messages.
	std::function<void(int, const string&)> dataChannelMessageHandler([&](
		int peerId,
		const std::string& message)
	{
		// Returns if the remote peer data hasn't been initialized.
		if (g_remotePeersData.find(peerId) == g_remotePeersData.end())
		{
			return;
		}

		char type[256];
		char body[1024];
		Json::Reader reader;
		Json::Value msg = NULL;
		reader.parse(message, msg, false);
		std::shared_ptr<RemotePeerData> peerData = g_remotePeersData[peerId];
		if (msg.isMember("type") && msg.isMember("body"))
		{
			strcpy(type, msg.get("type", "").asCString());
			strcpy(body, msg.get("body", "").asCString());
			std::istringstream datastream(body);
			std::string token;
			if (strcmp(type, "stereo-rendering") == 0 && !peerData->renderTexture)
			{
				getline(datastream, token, ',');
				peerData->isStereo = stoi(token) == 1;
				InitializeRenderTexture(
					peerData.get(),
					fullServerConfig->server_config->server_config.width,
					fullServerConfig->server_config->server_config.height,
					peerData->isStereo);

				InitializeDepthStencilTexture(
					peerData.get(),
					fullServerConfig->server_config->server_config.width,
					fullServerConfig->server_config->server_config.height,
					peerData->isStereo);

				if (!peerData->isStereo)
				{
					peerData->eyeVector = g_cubeRenderer->GetDefaultEyeVector();
					peerData->lookAtVector = g_cubeRenderer->GetDefaultLookAtVector();
					peerData->upVector = g_cubeRenderer->GetDefaultUpVector();
					peerData->tick = GetTickCount64();
				}
			}
			else if (strcmp(type, "camera-transform-lookat") == 0)
			{
				// Eye point.
				getline(datastream, token, ',');
				float eyeX = stof(token);
				getline(datastream, token, ',');
				float eyeY = stof(token);
				getline(datastream, token, ',');
				float eyeZ = stof(token);

				// Focus point.
				getline(datastream, token, ',');
				float focusX = stof(token);
				getline(datastream, token, ',');
				float focusY = stof(token);
				getline(datastream, token, ',');
				float focusZ = stof(token);

				// Up vector.
				getline(datastream, token, ',');
				float upX = stof(token);
				getline(datastream, token, ',');
				float upY = stof(token);
				getline(datastream, token, ',');
				float upZ = stof(token);

				peerData->lookAtVector = { focusX, focusY, focusZ, 0.f };
				peerData->upVector = { upX, upY, upZ, 0.f };
				peerData->eyeVector = { eyeX, eyeY, eyeZ, 0.f };
				peerData->isNew = true;
			}
			else if (strcmp(type, "camera-transform-stereo") == 0)
			{
				// Parses the left projection matrix.
				DirectX::XMFLOAT4X4 projectionMatrixLeft;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						projectionMatrixLeft.m[i][j] = stof(token);
					}
				}

				// Parses the left view matrix.
				DirectX::XMFLOAT4X4 viewMatrixLeft;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						viewMatrixLeft.m[i][j] = stof(token);
					}
				}

				// Parses the right projection matrix.
				DirectX::XMFLOAT4X4 projectionMatrixRight;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						projectionMatrixRight.m[i][j] = stof(token);
					}
				}

				// Parses the right view matrix.
				DirectX::XMFLOAT4X4 viewMatrixRight;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						viewMatrixRight.m[i][j] = stof(token);
					}
				}

				peerData->projectionMatrixLeft = projectionMatrixLeft;
				peerData->viewMatrixLeft = viewMatrixLeft;
				peerData->projectionMatrixRight = projectionMatrixRight;
				peerData->viewMatrixRight= viewMatrixRight;
				peerData->isNew = true;
			}
			else if (strcmp(type, "camera-transform-stereo-prediction") == 0)
			{
				// Parses the left projection matrix.
				DirectX::XMFLOAT4X4 projectionMatrixLeft;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						projectionMatrixLeft.m[i][j] = stof(token);
					}
				}

				// Parses the left view matrix.
				DirectX::XMFLOAT4X4 viewMatrixLeft;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						viewMatrixLeft.m[i][j] = stof(token);
					}
				}

				// Parses the right projection matrix.
				DirectX::XMFLOAT4X4 projectionMatrixRight;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						projectionMatrixRight.m[i][j] = stof(token);
					}
				}

				// Parses the right view matrix.
				DirectX::XMFLOAT4X4 viewMatrixRight;
				for (int i = 0; i < 4; i++)
				{
					for (int j = 0; j < 4; j++)
					{
						getline(datastream, token, ',');
						viewMatrixRight.m[i][j] = stof(token);
					}
				}

				// Parses the prediction timestamp.
				getline(datastream, token, ',');
				int64_t timestamp = stoll(token);
				if (timestamp != peerData->lastTimestamp)
				{
					peerData->lastTimestamp = timestamp;
					peerData->projectionMatrixLeft = projectionMatrixLeft;
					peerData->viewMatrixLeft = viewMatrixLeft;
					peerData->projectionMatrixRight = projectionMatrixRight;
					peerData->viewMatrixRight = viewMatrixRight;
					peerData->isNew = true;
				}
			}
		}
	});

	// Sets data channel message handler.
	cond.SetDataChannelMessageHandler(dataChannelMessageHandler);

	// For system service, automatically connect to the signaling server.
	if (fullServerConfig->server_config->server_config.system_service)
	{
		cond.StartLogin(fullServerConfig->webrtc_config->server_uri.c_str(),
			fullServerConfig->webrtc_config->port);
	}

	// Main loop.
	MSG msg = { 0 };
	while (!stopping && WM_QUIT != msg.message)
	{
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (fullServerConfig->server_config->server_config.system_service ||
				!wnd.PreTranslateMessage(&msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		else
		{
			for each (auto pair in cond.Peers())
			{
				auto peer = (DirectXPeerConductor*)pair.second.get();

				// Retrieves remote peer data from map, create new if needed.
				std::shared_ptr<RemotePeerData> peerData;
				auto it = g_remotePeersData.find(peer->Id());
				if (it == g_remotePeersData.end())
				{
					peerData.reset(new RemotePeerData());
					peerData->startTick = GetTickCount64();
					g_remotePeersData[peer->Id()] = peerData;
				}
				else
				{
					peerData = it->second;
				}

				if (!peerData->renderTexture)
				{
					// Forces non-stereo mode initialization.
					if (GetTickCount64() - peerData->startTick >= STEREO_FLAG_WAIT_TIME)
					{
						InitializeRenderTexture(
							peerData.get(),
							fullServerConfig->server_config->server_config.width,
							fullServerConfig->server_config->server_config.height,
							false);

						InitializeDepthStencilTexture(
							peerData.get(),
							fullServerConfig->server_config->server_config.width,
							fullServerConfig->server_config->server_config.height,
							false);

						peerData->isStereo = false;
						peerData->eyeVector = g_cubeRenderer->GetDefaultEyeVector();
						peerData->lookAtVector = g_cubeRenderer->GetDefaultLookAtVector();
						peerData->upVector = g_cubeRenderer->GetDefaultUpVector();
						peerData->tick = GetTickCount64();
					}
				}
				else
				{
					g_deviceResources->SetStereo(peerData->isStereo);
					if (!peerData->isStereo)
					{
						// FPS limiter.
						const int interval = 1000 / nvEncConfig->capture_fps;
						ULONGLONG timeElapsed = GetTickCount64() - peerData->tick;
						if (timeElapsed >= interval)
						{
							peerData->tick = GetTickCount64() - timeElapsed + interval;
							g_cubeRenderer->SetPosition(float3({ 0.f, 0.f, 0.f }));
							g_cubeRenderer->UpdateView(
								peerData->eyeVector,
								peerData->lookAtVector,
								peerData->upVector);

							g_cubeRenderer->Render(peerData->renderTargetView.Get());
							peer->SendFrame(peerData->renderTexture.Get());
						}
					}
					// In stereo rendering mode, we only update frame whenever
					// receiving any input data.
					else if (peerData->isNew)
					{
						g_cubeRenderer->SetPosition(float3({ 0.f, 0.f, FOCUS_POINT }));

						DirectX::XMFLOAT4X4 leftMatrix;
						XMStoreFloat4x4(
							&leftMatrix,
							XMLoadFloat4x4(&peerData->projectionMatrixLeft) * XMLoadFloat4x4(&peerData->viewMatrixLeft));

						DirectX::XMFLOAT4X4 rightMatrix;
						XMStoreFloat4x4(
							&rightMatrix,
							XMLoadFloat4x4(&peerData->projectionMatrixRight) * XMLoadFloat4x4(&peerData->viewMatrixRight));

						g_cubeRenderer->UpdateView(leftMatrix, rightMatrix);
						g_cubeRenderer->Render(peerData->renderTargetView.Get());
						peer->SendFrame(peerData->renderTexture.Get(), peerData->lastTimestamp);
						peerData->isNew = false;
					}
				}
			}
		}
	}

	// Cleanup.
	rtc::CleanupSSL();
	delete g_cubeRenderer;
	delete g_deviceResources;

	return 0;
}

extern "C" 
{
	__declspec(dllexport) void __stdcall  TestStreamingDLLInterface(ID3D11Device* device, ID3D11Texture2D* rt, 
		void __stdcall MouseEventCallback(char*, char*))
	{

		auto fullServerConfig = GlobalObject<FullServerConfig>::Get();
		auto nvEncConfig = GlobalObject<NvEncConfig>::Get();
		nvEncConfig->capture_fps = 60;

		D3D11_TEXTURE2D_DESC desc;
		rt->GetDesc(&desc);

		if (fullServerConfig)
		{
			fullServerConfig->webrtc_config = GlobalObject<StreamingToolkit::WebRTCConfig>::Get();
			fullServerConfig->webrtc_config->server_uri = "http://localhost";
			fullServerConfig->webrtc_config->port = 3000;
			fullServerConfig->webrtc_config->ice_configuration = "none";
			fullServerConfig->webrtc_config->heartbeat = 5000;

			fullServerConfig->server_config = GlobalObject<StreamingToolkit::ServerConfig>::Get();
			fullServerConfig->server_config->server_config.width = desc.Width;
			fullServerConfig->server_config->server_config.height = desc.Height;
			fullServerConfig->server_config->server_config.system_capacity = -1;
			fullServerConfig->server_config->server_config.system_service = false;
			fullServerConfig->server_config->server_config.auto_call = false;
			fullServerConfig->server_config->server_config.auto_connect = false;
		}

		
		rtc::EnsureWinsockInit();
		rtc::Win32Thread w32_thread;
		rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);

		/*
		// Initializes the device resources.
		g_deviceResources = new DeviceResources();
		*/

		rtc::InitializeSSL();

		// Initializes the conductor.
		g_cond = new DirectXMultiPeerConductor(fullServerConfig, device);

		// Handles data channel messages.
		std::function<void(int, const string&)> dataChannelMessageHandler([&](int peerId, const std::string& message)
		{
			// Returns if the remote peer data hasn't been initialized.
			if (g_remotePeersData.find(peerId) == g_remotePeersData.end())
			{
				return;
			}

			char type[256];
			char body[1024];
			Json::Reader reader;
			Json::Value msg = NULL;
			reader.parse(message, msg, false);
			std::shared_ptr<RemotePeerData> peerData = g_remotePeersData[peerId];

			if (msg.isMember("type") && msg.isMember("body"))
			{
				strcpy(type, msg.get("type", "").asCString());
				strcpy(body, msg.get("body", "").asCString());
			}




			MouseEventCallback(type, body);
		});
		
		// Sets data channel message handler.
		g_cond->SetDataChannelMessageHandler(dataChannelMessageHandler);

		g_cond->StartLogin("http://localhost", 3000);
				
		MSG msg = { 0 };
		while (WM_QUIT != msg.message)
		{
			if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{				
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			}			
		}
	}
	
	__declspec(dllexport) void __stdcall  RenderFrame(ID3D11Device* device, ID3D11Texture2D* rt)
	{
		if (!g_cond)
			return;

		for each (auto pair in g_cond->Peers())
		{
			auto peer = (DirectXPeerConductor*)pair.second.get();

			// Retrieves remote peer data from map, create new if needed.
			std::shared_ptr<RemotePeerData> peerData;
			auto it = g_remotePeersData.find(peer->Id());
			if (it == g_remotePeersData.end())
			{
				peerData.reset(new RemotePeerData());
				peerData->startTick = GetTickCount64();
				g_remotePeersData[peer->Id()] = peerData;
			}
			else
			{
				peerData = it->second;
			}

			// FPS limiter.
			ULONGLONG timeElapsed = GetTickCount64() - peerData->tick;
			peerData->tick = GetTickCount64() - timeElapsed;
			peer->SendFrame(rt);
		}
	}
}

//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
BOOL WINAPI DllMain(
	HINSTANCE hinstDLL,  // handle to DLL module
	DWORD fdwReason,     // reason for calling function
	LPVOID lpReserved)  // reserved
{
	// Perform actions based on the reason for calling.
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		// Initialize once for each new process.
		// Return FALSE to fail DLL load.
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;

	case DLL_PROCESS_DETACH:
		// Perform any necessary cleanup.
		break;
	}
	return TRUE;  // Successful DLL_PROCESS_ATTACH.
}

