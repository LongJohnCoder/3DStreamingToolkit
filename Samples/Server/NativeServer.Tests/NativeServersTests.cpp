#include "pch.h"

#include <comdef.h>
#include <comutil.h>
#include <gtest\gtest.h>
#include <Wbemidl.h>
#include <wchar.h>

#include "CppUnitTest.h"
#include "DeviceResources.h"
#include "directx_buffer_capturer.h"
#include "opengl_buffer_capturer.h"
#include "server_main_window.h"
#include "third_party\libyuv\include\libyuv.h"
#include "third_party\nvpipe\nvpipe.h"
#include "webrtc.h"
#include "webrtcH264.h"

#pragma comment(lib, "wbemuuid.lib")

using namespace Microsoft::WRL;
using namespace DX;
using namespace StreamingToolkit;
using namespace webrtc;

// Tests out initializing the H264 encoder.
TEST(EncoderTests, CanInitializeWithDefaultParameters)
{
	auto encoder = new H264EncoderImpl(cricket::VideoCodec("H264"));
	VideoCodec codecSettings;
	SetDefaultCodecSettings(&codecSettings);
	ASSERT_TRUE(encoder->InitEncode(
		&codecSettings, kNumCores, kMaxPayloadSize) == WEBRTC_VIDEO_CODEC_OK);

	// Test correct release of encoder
	ASSERT_TRUE(encoder->Release() == WEBRTC_VIDEO_CODEC_OK);
	delete encoder;
}

// --------------------------------------------------------------
// see https://github.com/google/googletest/blob/master/googletest/docs/AdvancedGuide.md#temporarily-enabling-disabled-tests
// for how to run this test (disabled by default, as this fails without nvidia gpu support)
// --------------------------------------------------------------
// Tests out retrieving the compatible NVIDIA driver version.
TEST(EncoderTests, DISABLED_HasCompatibleGPUAndDriver)
{
	HRESULT hres;

	// Step 1: --------------------------------------------------
	// Initialize COM. ------------------------------------------

	hres = CoInitializeEx(0, COINIT_MULTITHREADED);


	// Step 2: --------------------------------------------------
	// Set general COM security levels --------------------------

	hres = CoInitializeSecurity(
		NULL,
		-1,                          // COM authentication
		NULL,                        // Authentication services
		NULL,                        // Reserved
		RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication 
		RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation  
		NULL,                        // Authentication info
		EOAC_NONE,                   // Additional capabilities 
		NULL                         // Reserved
	);

	// Step 3: ---------------------------------------------------
	// Obtain the initial locator to WMI -------------------------

	IWbemLocator *pLoc = NULL;

	hres = CoCreateInstance(
		CLSID_WbemLocator,
		0,
		CLSCTX_INPROC_SERVER,
		IID_IWbemLocator, (LPVOID *)&pLoc);

	ASSERT_FALSE(FAILED(hres));

	// Step 4: -----------------------------------------------------
	// Connect to WMI through the IWbemLocator::ConnectServer method

	IWbemServices *pSvc = NULL;

	// Connect to the root\cimv2 namespace with
	// the current user and obtain pointer pSvc
	// to make IWbemServices calls.
	hres = pLoc->ConnectServer(
		_bstr_t(L"ROOT\\CIMV2"), // Object path of WMI namespace
		NULL,                    // User name. NULL = current user
		NULL,                    // User password. NULL = current
		0,                       // Locale. NULL indicates current
		NULL,                    // Security flags.
		0,                       // Authority (for example, Kerberos)
		0,                       // Context object 
		&pSvc                    // pointer to IWbemServices proxy
	);

	ASSERT_FALSE(FAILED(hres));

	// Step 5: --------------------------------------------------
	// Set security levels on the proxy -------------------------

	hres = CoSetProxyBlanket(
		pSvc,                        // Indicates the proxy to set
		RPC_C_AUTHN_WINNT,           // RPC_C_AUTHN_xxx
		RPC_C_AUTHZ_NONE,            // RPC_C_AUTHZ_xxx
		NULL,                        // Server principal name 
		RPC_C_AUTHN_LEVEL_CALL,      // RPC_C_AUTHN_LEVEL_xxx 
		RPC_C_IMP_LEVEL_IMPERSONATE, // RPC_C_IMP_LEVEL_xxx
		NULL,                        // client identity
		EOAC_NONE                    // proxy capabilities 
	);

	ASSERT_FALSE(FAILED(hres));

	// Step 6: --------------------------------------------------
	// Use the IWbemServices pointer to make requests of WMI ----

	// For example, get the name of the operating system
	IEnumWbemClassObject* pEnumerator = NULL;
	hres = pSvc->ExecQuery(
		bstr_t("WQL"),
		bstr_t("SELECT * FROM Win32_VideoController"),
		WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
		NULL,
		&pEnumerator);

	ASSERT_FALSE(FAILED(hres));

	// Step 7: -------------------------------------------------
	// Get the data from the query in step 6 -------------------

	IWbemClassObject *pclsObj = NULL;
	ULONG uReturn = 0;

	VARIANT driverNumber; //Store the driver version installed
	bool NvidiaPresent = false; //Flag for Nvidia card being present

	while (pEnumerator)
	{
		HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1,
			&pclsObj, &uReturn);

		if (0 == uReturn)
		{
			break;
		}

		VARIANT vtProp;
		//Finds the manufacturer of the card
		hr = pclsObj->Get(L"AdapterCompatibility", 0, &vtProp, 0, 0);

		//Find the nvidia card
		if (!wcscmp(vtProp.bstrVal, L"NVIDIA")) 
		{
			//Set the Nvidia card flag to true
			NvidiaPresent = true;

			hr = pclsObj->Get(L"DriverVersion", 0, &driverNumber, 0, 0);
			wchar_t *currentDriver = driverNumber.bstrVal;
			size_t len = wcslen(currentDriver);

			//Major version number of the card is found at the -7th index
			std::wstring majorVersion(currentDriver, len - 6, 1);

			//All drivers from 3.0 onwards support nvencode
			ASSERT_TRUE(std::stoi(majorVersion) > 2);
		}

		VariantClear(&vtProp);
		pclsObj->Release();
	}
	
	//Make sure that we entered the loop
	ASSERT_TRUE(NvidiaPresent);

	//Clean Up
	VariantClear(&driverNumber);
}

// --------------------------------------------------------------
// see https://github.com/google/googletest/blob/master/googletest/docs/AdvancedGuide.md#temporarily-enabling-disabled-tests
// for how to run this test (disabled by default, as this fails without nvidia gpu support)
// --------------------------------------------------------------
// Tests out hardware encoder initialization.
TEST(EncoderTests, DISABLED_HardwareEncodingIsEnabled)
{
	//Using default settings from webrtc documentation
	const nvpipe_codec codec = NVPIPE_H264_NV;
	const uint32_t width = 1280;
	const uint32_t height = 720;
	const uint64_t bitrate = width * height * 30 * 4 * 0.07;

	//Using autos from here on for simplicity
	auto hGetProcIDDLL = LoadLibrary(L"Nvpipe.dll");

	auto create_nvpipe_encoder = (nvpipe_create_encoder)GetProcAddress(hGetProcIDDLL, "nvpipe_create_encoder");
	auto destroy_nvpipe_encoder = (nvpipe_destroy)GetProcAddress(hGetProcIDDLL, "nvpipe_destroy");
	auto encode_nvpipe = (nvpipe_encode)GetProcAddress(hGetProcIDDLL, "nvpipe_encode");
	auto reconfigure_nvpipe = (nvpipe_bitrate)GetProcAddress(hGetProcIDDLL, "nvpipe_bitrate");

	//Check to ensure that each of the functions loaded correctly (DLL exists, is functional)
	ASSERT_TRUE(create_nvpipe_encoder);
	ASSERT_TRUE(destroy_nvpipe_encoder);
	ASSERT_TRUE(encode_nvpipe);
	ASSERT_TRUE(reconfigure_nvpipe);

	//Check to ensure that the encoder can be created correctly
	auto encoder = create_nvpipe_encoder(codec, bitrate, 90, NVENC_INFINITE_GOPLENGTH, 1, false);
	ASSERT_TRUE(encoder);

	//Ensure that the encoder can be destroyed correctly
	destroy_nvpipe_encoder(encoder);

	FreeLibrary((HMODULE)hGetProcIDDLL);
}

// Tests out encoding a video frame using hardware encoder.
TEST(EncoderTests, CanEncodeCorrectly)
{
	auto h264TestImpl = new H264TestImpl();
	h264TestImpl->SetEncoderHWEnabled(true);
	rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer(
		h264TestImpl->input_frame_.get()->video_frame_buffer());

	size_t bufferSize = buffer->width() * buffer->height() * 4;
	size_t RowBytes = buffer->width() * 4;
	uint8_t* rgbBuffer = new uint8_t[bufferSize];

	// Convert input frame to RGB
	libyuv::I420ToARGB(buffer->GetI420()->DataY(), buffer->GetI420()->StrideY(),
		buffer->GetI420()->DataU(), buffer->GetI420()->StrideU(),
		buffer->GetI420()->DataV(), buffer->GetI420()->StrideV(),
		rgbBuffer,
		RowBytes,
		buffer->width(), buffer->height());

	// Set RGB frame 
	h264TestImpl->input_frame_.get()->set_frame_buffer(rgbBuffer);

	// Encode frame
	ASSERT_TRUE(h264TestImpl->encoder_->Encode(*h264TestImpl->input_frame_, nullptr, nullptr) == WEBRTC_VIDEO_CODEC_OK);
	EncodedImage encodedFrame;

	// Extract encoded_frame from the encoder
	ASSERT_TRUE(h264TestImpl->WaitForEncodedFrame(&encodedFrame));

	// Check if we have a complete frame with lengh > 0
	ASSERT_TRUE(encodedFrame._completeFrame);
	ASSERT_TRUE(encodedFrame._length > 0);

	// Test correct release of encoder
	ASSERT_TRUE(h264TestImpl->encoder_->Release() == WEBRTC_VIDEO_CODEC_OK);

	delete[] rgbBuffer;
	rgbBuffer = NULL;
}
