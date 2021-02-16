#include <obs-module.h>
#include <obs-frontend-api.h>
#include <stdio.h>

#include <media-io/audio-math.h>
#include <math.h>

#include <windows.h>
#include "circle-buffer.h"
#include "VoicemeeterRemote.h"

#include <QMainWindow>
#include <QMenu>
#include <QString>
#include <QMenuBar>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-voicemeeter", "en-US")

#define blog(level, msg, ...) \
	blog(level, "obs-voicemeeter: " msg, ##__VA_ARGS__)

#define MT_ obs_module_text

#ifndef MAX_AUDIO_SIZE
#ifndef AUDIO_OUTPUT_FRAMES
#define AUDIO_OUTPUT_FRAMES 1024
#endif
#define MAX_AUDIO_SIZE (AUDIO_OUTPUT_FRAMES * sizeof(float))
#endif // !MAX_AUDIO_SIZE

enum audio_format get_planar_format(audio_format format)
{
	if (is_audio_planar(format))
		return format;

	switch (format) {
	case AUDIO_FORMAT_U8BIT:
		return AUDIO_FORMAT_U8BIT_PLANAR;
	case AUDIO_FORMAT_16BIT:
		return AUDIO_FORMAT_16BIT_PLANAR;
	case AUDIO_FORMAT_32BIT:
		return AUDIO_FORMAT_32BIT_PLANAR;
	case AUDIO_FORMAT_FLOAT:
		return AUDIO_FORMAT_FLOAT_PLANAR;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

enum voicemeeter_type {
	voicemeeter_normal = 1,
	voicemeeter_banana = 2,
	voicemeeter_potato = 3,
};

enum voicemeeter_hook {
	voicemeeter_insert_in = 0,
	voicemeeter_insert_out,
	voicemeeter_main
};

static char uninstDirKey[] =
	"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

#define INSTALLER_UNINST_KEY "VB:Voicemeeter {17359A74-1236-5467}"

struct VBVMR_T_AUDIOBUFFER_TS {
	VBVMR_T_AUDIOBUFFER data;
	uint64_t ts;
};

static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferInsertIn;
static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferInsertOut;
static StreamableBuffer<VBVMR_T_AUDIOBUFFER_TS> OBSBufferMain;

void RemoveNameInPath(char *szPath)
{
	long ll;
	ll = (long)strlen(szPath);
	while ((ll > 0) && (szPath[ll] != '\\'))
		ll--;
	if (szPath[ll] == '\\')
		szPath[ll] = 0;
}

#ifndef KEY_WOW64_32KEY
#define KEY_WOW64_32KEY 0x0200
#endif

BOOL __cdecl RegistryGetVoicemeeterFolderA(char *szDir)
{
	char szKey[256];
	char sss[1024];
	DWORD nnsize = 1024;
	HKEY hkResult;
	LONG rep;
	DWORD pptype = REG_SZ;
	sss[0] = 0;

	// build Voicemeeter uninstallation key
	strcpy(szKey, uninstDirKey);
	strcat(szKey, "\\");
	strcat(szKey, INSTALLER_UNINST_KEY);

	// open key
	rep = RegOpenKeyExA(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ, &hkResult);
	if (rep != ERROR_SUCCESS) {
		// if not present we consider running in 64bit mode and force to read 32bit registry
		rep = RegOpenKeyExA(HKEY_LOCAL_MACHINE, szKey, 0,
				    KEY_READ | KEY_WOW64_32KEY, &hkResult);
	}

	if (rep != ERROR_SUCCESS) {
		blog(LOG_ERROR, "error %i reading registry", rep);
		return FALSE;
	}
	// read uninstall from program path
	rep = RegQueryValueExA(hkResult, "UninstallString", 0, &pptype,
			       (unsigned char *)sss, &nnsize);
	RegCloseKey(hkResult);

	if (pptype != REG_SZ) {
		blog(LOG_INFO, "pptype %u, %u", pptype, REG_SZ);
		return FALSE;
	}
	if (rep != ERROR_SUCCESS) {
		blog(LOG_ERROR, "error %i getting value", rep);
		return FALSE;
	}
	// remove name to get the path only
	RemoveNameInPath(sss);
	if (nnsize > 512)
		nnsize = 512;
	strncpy(szDir, sss, nnsize);

	return TRUE;
}

static T_VBVMR_INTERFACE iVMR;
static HMODULE G_H_Module = NULL;
static long application = 0;
struct version32_t {
	union {
		struct {
			uint8_t v1, v2, v3, v4;
		};
		long v;
	};
};
static version32_t version = {0};
static long vb_type;
static std::vector<int32_t> validInputs = {0, 12, 22, 34};
static std::vector<int32_t> validOutputs = {0, 16, 40, 64};
static std::vector<int32_t> validMains = {0, 28, 62, 98};

typedef std::pair<speaker_layout, std::string> vb_layout;
typedef std::pair<speaker_layout, std::vector<std::string>> channel_layout;
typedef std::pair<int, std::string> vb_layout_map;

static std::vector<vb_layout> inputLayouts[4] = {
	{
		/*Unknown*/
	},
	{
		{SPEAKERS_STEREO, "Strip 1"},
		{SPEAKERS_STEREO, "Strip 2"},
		{SPEAKERS_7POINT1, "Virtual Input"},
	},
	{
		{SPEAKERS_STEREO, "Strip 1"},
		{SPEAKERS_STEREO, "Strip 2"},
		{SPEAKERS_STEREO, "Strip 3"},
		{SPEAKERS_7POINT1, "Virtual Input"},
		{SPEAKERS_7POINT1, "Virtual Input (AUX)"},
	},
	{
		{SPEAKERS_STEREO, "Strip 1"},
		{SPEAKERS_STEREO, "Strip 2"},
		{SPEAKERS_STEREO, "Strip 3"},
		{SPEAKERS_STEREO, "Strip 4"},
		{SPEAKERS_STEREO, "Strip 5"},
		{SPEAKERS_7POINT1, "Virtual Input"},
		{SPEAKERS_7POINT1, "Virtual Input (AUX)"},
		{SPEAKERS_7POINT1, "Virtual Input 8"},
	}};

static std::vector<vb_layout> outputLayouts[4] = {
	{
		/*Unknown*/
	},
	{
		{SPEAKERS_7POINT1, "Output A1 / A2"},
		{SPEAKERS_7POINT1, "Virtual Output"},
	},
	{
		{SPEAKERS_7POINT1, "Output A1"},
		{SPEAKERS_7POINT1, "Output A2"},
		{SPEAKERS_7POINT1, "Output A3"},
		{SPEAKERS_7POINT1, "Virtual Output B1"},
		{SPEAKERS_7POINT1, "Virtual Output B2"},
	},
	{
		{SPEAKERS_7POINT1, "Output A1"},
		{SPEAKERS_7POINT1, "Output A2"},
		{SPEAKERS_7POINT1, "Output A3"},
		{SPEAKERS_7POINT1, "Output A4"},
		{SPEAKERS_7POINT1, "Output A5"},
		{SPEAKERS_7POINT1, "Virtual Output B1"},
		{SPEAKERS_7POINT1, "Virtual Output B2"},
		{SPEAKERS_7POINT1, "Virtual Output B3"},
	}};
static std::vector<vb_layout> mainLayouts[4] = {};

static std::vector<channel_layout> channelLayouts = {
	{SPEAKERS_STEREO, {"(L)", "(R)"}},
	{SPEAKERS_7POINT1,
	 {"(L)", "(R)", "(C)", "(LFE)", "(SL)", "(SR)", "(BL)", "(BR)"}},
};
/*mainLayouts is made by appending outputLayouts to inputLayouts*/

static std::vector<vb_layout_map> inputMap[4];
static std::vector<vb_layout_map> outputMap[4];
static std::vector<vb_layout_map> mainMap[4];

static void copyToBuffer(VBVMR_T_AUDIOBUFFER_TS &buf,
			 VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	if (used) {
		size_t oldBufSize = out.data.audiobuffer_nbs * sizeof(float);
		if (bufSize > oldBufSize) {
			for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
				bfree(out.data.audiobuffer_r[i]);
				out.data.audiobuffer_r[i] = (float *)bmemdup(
					buf.data.audiobuffer_r[i], bufSize);
			}
		} else {
			for (int i = 0; i < buf.data.audiobuffer_nbi; i++)
				memcpy(out.data.audiobuffer_r[i],
				       buf.data.audiobuffer_r[i], bufSize);
		}
	} else {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++)
			out.data.audiobuffer_r[i] = (float *)bmemdup(
				buf.data.audiobuffer_r[i], bufSize);
	}
	out.data.audiobuffer_nbi = buf.data.audiobuffer_nbi;
	out.data.audiobuffer_nbo = buf.data.audiobuffer_nbo;
	out.data.audiobuffer_nbs = buf.data.audiobuffer_nbs;
	out.data.audiobuffer_sr = buf.data.audiobuffer_sr;
	out.ts = buf.ts;
}

static void writeInsertAudio(VBVMR_T_AUDIOBUFFER_TS &buf,
			     VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validInputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i], buf.data.audiobuffer_r[i],
		       bufSize);
}

static void writeInsertOutAudio(VBVMR_T_AUDIOBUFFER_TS &buf,
				VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validOutputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i], buf.data.audiobuffer_r[i],
		       bufSize);
}

static void writeMainAudio(VBVMR_T_AUDIOBUFFER_TS &buf,
			   VBVMR_T_AUDIOBUFFER_TS &out, bool used)
{
	copyToBuffer(buf, out, used);
	size_t bufSize = buf.data.audiobuffer_nbs * sizeof(float);
	/*pass-through*/
	for (int i = 0; i < validOutputs[vb_type]; i++)
		memcpy(buf.data.audiobuffer_w[i],
		       buf.data.audiobuffer_r[i + validInputs[vb_type]],
		       bufSize);
}

int vm_start();
int vm_stop();
int vm_login();
int vm_logout();
int vm_register();
int vm_unregister();
int vm_launch();

static long audioCallback(void *lpUser, long nCommand, void *lpData, long nnn)
{
	uint64_t tStamp = os_gettime_ns();
	VBVMR_T_AUDIOBUFFER_TS audioBuf;

	switch (nCommand) {
	case VBVMR_CBCOMMAND_STARTING:
		/*update the application version (in case user opens alternate version)*/
		iVMR.VBVMR_GetVoicemeeterType(&vb_type);
		break;
	case VBVMR_CBCOMMAND_CHANGE:
		iVMR.VBVMR_GetVoicemeeterType(&vb_type);
		QTimer::singleShot(100, []() {
			vm_stop();
			vm_start();
		});
		break;
	case VBVMR_CBCOMMAND_ENDING:
		UNUSED_PARAMETER(lpUser);
		UNUSED_PARAMETER(nnn);
		break;
	case VBVMR_CBCOMMAND_BUFFER_IN:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferInsertIn.Write(audioBuf, writeInsertAudio);
		break;
	case VBVMR_CBCOMMAND_BUFFER_OUT:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferInsertOut.Write(audioBuf, writeInsertOutAudio);
		break;
	case VBVMR_CBCOMMAND_BUFFER_MAIN:
		audioBuf.data = *((VBVMR_LPT_AUDIOBUFFER)lpData);
		audioBuf.ts = tStamp;
		OBSBufferMain.Write(audioBuf, writeMainAudio);
		break;
	}
	return 0;
}

int vm_login()
{
	int ret = iVMR.VBVMR_Login();
	switch (ret) {
	case 0:
		blog(LOG_INFO, "client logged in");
		break;
	case 1:
		blog(LOG_INFO, "voicemeeter has not been launched");
		break;
	case -1:
		blog(LOG_ERROR, "cannot get client");
		break;
	case -2:
		blog(LOG_ERROR, "unexpected login");
		break;
	default:
		blog(LOG_ERROR, "unexpected error %i logging in", ret);
		break;
	}
	return ret;
}

int vm_logout()
{
	int ret = iVMR.VBVMR_Logout();
	switch (ret) {
	case 0:
		blog(LOG_INFO, "client logged out");
		break;
	default:
		blog(LOG_INFO, "voicemeeter is not installed");
		break;
	}
	return ret;
}

int vm_launch()
{
	int ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_potato);
	if (ret == 0) {
		blog(LOG_INFO, "successfully opened potato");
		return ret;
	}

	ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_banana);
	if (ret == 0) {
		blog(LOG_INFO, "successfully opened banana");
		return ret;
	}

	ret = iVMR.VBVMR_RunVoicemeeter(voicemeeter_normal);
	if (ret == 0) {
		blog(LOG_INFO, "successfully opened basic");
		return ret;
	}

	blog(LOG_INFO, "failed to open voicemeeter");
	return ret;
}

int vm_start()
{
	int ret = iVMR.VBVMR_AudioCallbackStart();
	switch (ret) {
	case 0:
		blog(LOG_INFO, "successfully started audio callback");
		break;
	case -1:
		blog(LOG_INFO, "failed to start audio callback");
		break;
	case -2:
		blog(LOG_INFO, "no audio callback registered");
		break;
	}
	return ret;
}

int vm_stop()
{
	int ret = iVMR.VBVMR_AudioCallbackStop();
	switch (ret) {
	case 0:
		blog(LOG_INFO, "successfully stopped audio callback");
		break;
	case -1:
		blog(LOG_INFO, "failed to stop audio callback");
		break;
	case -2:
		blog(LOG_INFO, "no audio callback registered");
		break;
	}
	return ret;
}

int vm_register()
{
	long opts = VBVMR_AUDIOCALLBACK_IN | VBVMR_AUDIOCALLBACK_OUT |
		    VBVMR_AUDIOCALLBACK_MAIN;
	char application_name[64] = "obs-voicemeeter";
	int ret = iVMR.VBVMR_AudioCallbackRegister(opts, audioCallback, NULL,
						   application_name);
	switch (ret) {
	case 0:
		blog(LOG_INFO, "registered callback: %s", application_name);
		break;
	case -1:
		blog(LOG_ERROR, "error %i registering audio callback", ret);
		break;
	case 1:
		blog(LOG_ERROR,
		     "error %i registering audio callback: %s has already registered a callback",
		     ret, application_name);
		break;
	default:
		blog(LOG_ERROR, "unexpected code %i registering audio callback",
		     ret);
		break;
	}
	return ret;
}

int vm_unregister()
{
	int ret = iVMR.VBVMR_AudioCallbackUnregister();
	switch (ret) {
	case 0:
		break;
	case -1:
		blog(LOG_ERROR, "error %i unregistering audio callback", ret);
		break;
	case 1:
		blog(LOG_ERROR,
		     "error %i unregistering audio callback: already unregistered callback",
		     ret);
		break;
	default:
		blog(LOG_ERROR,
		     "unexpected code %i unregistering audio callback", ret);
		break;
	}
	return ret;
}

int vm_info()
{
	int ret = iVMR.VBVMR_GetVoicemeeterType((long *)&vb_type);
	if (ret != 0) {
		blog(LOG_ERROR, "could not get voicmeeter type");
		return ret;
	}
	ret = iVMR.VBVMR_GetVoicemeeterVersion((long *)&version.v);
	if (ret != 0) {
		blog(LOG_ERROR, "could not get voicmeeter version");
		return ret;
	}

	switch (vb_type) {
	case voicemeeter_potato:
		blog(LOG_INFO, "running voicemeeter potato %u.%u.%u.%u",
		     version.v4, version.v3, version.v2, version.v1);
		break;
	case voicemeeter_banana:
		blog(LOG_INFO, "running voicemeeter banana %u.%u.%u.%u",
		     version.v4, version.v3, version.v2, version.v1);
		break;
	case voicemeeter_normal:
		blog(LOG_INFO, "running voicemeeter %u.%u.%u.%u", version.v4,
		     version.v3, version.v2, version.v1);
		break;
	default:
		blog(LOG_INFO, "running voicemeeter (unknown) %u.%u.%u.%u",
		     version.v4, version.v3, version.v2, version.v1);
		break;
	}
	return ret;
}

int vm_deviceinfo()
{
	long deviceType;
	int ret;
	char deviceName[1024] = {0};
	char deviceId[1024] = {0};

	//get info about the devices available to voicemeeter
	int num = iVMR.VBVMR_Input_GetDeviceNumber();
	for (int i = 0; i < num; i++) {
		ret = iVMR.VBVMR_Input_GetDeviceDescA(
			i, &deviceType, &deviceName[0], &deviceId[0]);
		if (ret == 0) {
			switch (deviceType) {
			case VBVMR_DEVTYPE_MME:
				blog(LOG_INFO, "MME (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_WDM:
				blog(LOG_INFO, "WDM (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_KS:
				blog(LOG_INFO, "KS (%i): %s", i, deviceName);
				break;
			case VBVMR_DEVTYPE_ASIO:
				blog(LOG_INFO, "ASIO (%i): %s", i, deviceName);
				break;
			default:
				continue;
			}
		}
	}
	return num;
}

static void makeMainLayout(int mode)
{
	std::vector<vb_layout> main;
	main.reserve(inputLayouts[mode].size() + outputLayouts[mode].size());
	main.insert(main.end(), inputLayouts[mode].begin(),
		    inputLayouts[mode].end());
	main.insert(main.end(), outputLayouts[mode].begin(),
		    outputLayouts[mode].end());
	mainLayouts[mode] = main;
}

static void makeMap(int mode)
{
	int channel = -1;

	for (size_t i = 0; i < inputLayouts[mode].size(); i++) {
		channel += get_audio_channels(inputLayouts[mode][i].first);
		inputMap[mode].push_back(
			{channel, inputLayouts[mode][i].second});
	}

	channel = -1;
	for (size_t i = 0; i < outputLayouts[mode].size(); i++) {
		channel += get_audio_channels(outputLayouts[mode][i].first);
		outputMap[mode].push_back(
			{channel, outputLayouts[mode][i].second});
	}

	channel = -1;
	for (size_t i = 0; i < inputLayouts[mode].size(); i++) {
		channel += get_audio_channels(inputLayouts[mode][i].first);
		mainMap[mode].push_back(
			{channel, inputLayouts[mode][i].second});
	}
	for (size_t i = 0; i < outputLayouts[mode].size(); i++) {
		channel += get_audio_channels(outputLayouts[mode][i].first);
		mainMap[mode].push_back(
			{channel, outputLayouts[mode][i].second});
	}

	return;
}

static std::string getChannelName(int index, int mode)
{
	std::vector<vb_layout_map>::iterator it;
	std::vector<std::string> channels;
	std::string c = "";
	ptrdiff_t idx;
	int layout;
	int count;
	int offset;
	vb_layout_map search = {index, ""};
	switch (mode) {
	case voicemeeter_insert_in:
		it = std::lower_bound(
			inputMap[vb_type].begin(), inputMap[vb_type].end(),
			search, [](vb_layout_map left, vb_layout_map right) {
				return left.first < right.first;
			});
		if (it == inputMap[vb_type].end())
			return "";
		idx = std::distance(inputMap[vb_type].begin(), it);
		layout = inputLayouts[vb_type][idx].first;
		count = get_audio_channels((speaker_layout)layout);
		offset = (count - 1) - abs(it->first - index);
		for (size_t i = 0; i < channelLayouts.size(); i++) {
			if (channelLayouts[i].first == layout) {
				channels = channelLayouts[i].second;
				break;
			}
		}
		c = channels[offset];
		return it->second + " " + c;
	case voicemeeter_insert_out:
		it = std::lower_bound(
			outputMap[vb_type].begin(), outputMap[vb_type].end(),
			search, [](vb_layout_map left, vb_layout_map right) {
				return left.first < right.first;
			});
		if (it == outputMap[vb_type].end())
			return "";
		idx = std::distance(outputMap[vb_type].begin(), it);
		layout = outputLayouts[vb_type][idx].first;
		count = get_audio_channels((speaker_layout)layout);
		offset = (count - 1) - abs(it->first - index);
		for (size_t i = 0; i < channelLayouts.size(); i++) {
			if (channelLayouts[i].first == layout) {
				channels = channelLayouts[i].second;
				break;
			}
		}
		c = channels[offset];
		return it->second + " " + c;
	case voicemeeter_main:
		it = std::lower_bound(
			mainMap[vb_type].begin(), mainMap[vb_type].end(),
			search, [](vb_layout_map left, vb_layout_map right) {
				return left.first < right.first;
			});
		if (it == mainMap[vb_type].end())
			return "";
		idx = std::distance(outputMap[vb_type].begin(), it);
		layout = mainLayouts[vb_type][idx].first;
		count = get_audio_channels((speaker_layout)layout);
		offset = (count - 1) - abs(it->first - index);
		for (size_t i = 0; i < channelLayouts.size(); i++) {
			if (channelLayouts[i].first == layout) {
				channels = channelLayouts[i].second;
				break;
			}
		}
		c = channels[offset];
		return it->second + " " + c;
	default:
		break;
	}
	return "";
}

static long InitializeDLLInterfaces(void)
{
	char szDllName[1024] = {0};
	memset(&iVMR, 0, sizeof(T_VBVMR_INTERFACE));

	if (RegistryGetVoicemeeterFolderA(szDllName) == FALSE) {
		// voicemeeter not installed?
		blog(LOG_INFO, "voicemeeter does not appear to be installed");
		return -100;
	}

	//use right dll w/ bitness
	if (sizeof(void *) == 8)
		strcat(szDllName, "\\VoicemeeterRemote64.dll");
	else
		strcat(szDllName, "\\VoicemeeterRemote.dll");

	// Load Dll
	G_H_Module = LoadLibraryA(szDllName);
	if (G_H_Module == NULL) {
		blog(LOG_INFO, ".dll failed to load");
		return -101;
	}

	// Get function pointers
	iVMR.VBVMR_Login =
		(T_VBVMR_Login)GetProcAddress(G_H_Module, "VBVMR_Login");
	iVMR.VBVMR_Logout =
		(T_VBVMR_Logout)GetProcAddress(G_H_Module, "VBVMR_Logout");
	iVMR.VBVMR_RunVoicemeeter = (T_VBVMR_RunVoicemeeter)GetProcAddress(
		G_H_Module, "VBVMR_RunVoicemeeter");
	iVMR.VBVMR_GetVoicemeeterType =
		(T_VBVMR_GetVoicemeeterType)GetProcAddress(
			G_H_Module, "VBVMR_GetVoicemeeterType");
	iVMR.VBVMR_GetVoicemeeterVersion =
		(T_VBVMR_GetVoicemeeterVersion)GetProcAddress(
			G_H_Module, "VBVMR_GetVoicemeeterVersion");

	iVMR.VBVMR_IsParametersDirty =
		(T_VBVMR_IsParametersDirty)GetProcAddress(
			G_H_Module, "VBVMR_IsParametersDirty");
	iVMR.VBVMR_GetParameterFloat =
		(T_VBVMR_GetParameterFloat)GetProcAddress(
			G_H_Module, "VBVMR_GetParameterFloat");
	iVMR.VBVMR_GetParameterStringA =
		(T_VBVMR_GetParameterStringA)GetProcAddress(
			G_H_Module, "VBVMR_GetParameterStringA");
	iVMR.VBVMR_GetParameterStringW =
		(T_VBVMR_GetParameterStringW)GetProcAddress(
			G_H_Module, "VBVMR_GetParameterStringW");
	iVMR.VBVMR_GetLevel =
		(T_VBVMR_GetLevel)GetProcAddress(G_H_Module, "VBVMR_GetLevel");
	iVMR.VBVMR_GetMidiMessage = (T_VBVMR_GetMidiMessage)GetProcAddress(
		G_H_Module, "VBVMR_GetMidiMessage");

	iVMR.VBVMR_SetParameterFloat =
		(T_VBVMR_SetParameterFloat)GetProcAddress(
			G_H_Module, "VBVMR_SetParameterFloat");
	iVMR.VBVMR_SetParameters = (T_VBVMR_SetParameters)GetProcAddress(
		G_H_Module, "VBVMR_SetParameters");
	iVMR.VBVMR_SetParametersW = (T_VBVMR_SetParametersW)GetProcAddress(
		G_H_Module, "VBVMR_SetParametersW");
	iVMR.VBVMR_SetParameterStringA =
		(T_VBVMR_SetParameterStringA)GetProcAddress(
			G_H_Module, "VBVMR_SetParameterStringA");
	iVMR.VBVMR_SetParameterStringW =
		(T_VBVMR_SetParameterStringW)GetProcAddress(
			G_H_Module, "VBVMR_SetParameterStringW");

	iVMR.VBVMR_Output_GetDeviceNumber =
		(T_VBVMR_Output_GetDeviceNumber)GetProcAddress(
			G_H_Module, "VBVMR_Output_GetDeviceNumber");
	iVMR.VBVMR_Output_GetDeviceDescA =
		(T_VBVMR_Output_GetDeviceDescA)GetProcAddress(
			G_H_Module, "VBVMR_Output_GetDeviceDescA");
	iVMR.VBVMR_Output_GetDeviceDescW =
		(T_VBVMR_Output_GetDeviceDescW)GetProcAddress(
			G_H_Module, "VBVMR_Output_GetDeviceDescW");
	iVMR.VBVMR_Input_GetDeviceNumber =
		(T_VBVMR_Input_GetDeviceNumber)GetProcAddress(
			G_H_Module, "VBVMR_Input_GetDeviceNumber");
	iVMR.VBVMR_Input_GetDeviceDescA =
		(T_VBVMR_Input_GetDeviceDescA)GetProcAddress(
			G_H_Module, "VBVMR_Input_GetDeviceDescA");
	iVMR.VBVMR_Input_GetDeviceDescW =
		(T_VBVMR_Input_GetDeviceDescW)GetProcAddress(
			G_H_Module, "VBVMR_Input_GetDeviceDescW");

	iVMR.VBVMR_AudioCallbackRegister =
		(T_VBVMR_AudioCallbackRegister)GetProcAddress(
			G_H_Module, "VBVMR_AudioCallbackRegister");
	iVMR.VBVMR_AudioCallbackStart =
		(T_VBVMR_AudioCallbackStart)GetProcAddress(
			G_H_Module, "VBVMR_AudioCallbackStart");
	iVMR.VBVMR_AudioCallbackStop =
		(T_VBVMR_AudioCallbackStop)GetProcAddress(
			G_H_Module, "VBVMR_AudioCallbackStop");
	iVMR.VBVMR_AudioCallbackUnregister =
		(T_VBVMR_AudioCallbackUnregister)GetProcAddress(
			G_H_Module, "VBVMR_AudioCallbackUnregister");

	// Check pointers are valid
	if (iVMR.VBVMR_Login == NULL)
		return -1;
	if (iVMR.VBVMR_Logout == NULL)
		return -2;
	if (iVMR.VBVMR_RunVoicemeeter == NULL)
		return -2;
	if (iVMR.VBVMR_GetVoicemeeterType == NULL)
		return -3;
	if (iVMR.VBVMR_GetVoicemeeterVersion == NULL)
		return -4;
	if (iVMR.VBVMR_IsParametersDirty == NULL)
		return -5;
	if (iVMR.VBVMR_GetParameterFloat == NULL)
		return -6;
	if (iVMR.VBVMR_GetParameterStringA == NULL)
		return -7;
	if (iVMR.VBVMR_GetParameterStringW == NULL)
		return -8;
	if (iVMR.VBVMR_GetLevel == NULL)
		return -9;
	if (iVMR.VBVMR_SetParameterFloat == NULL)
		return -10;
	if (iVMR.VBVMR_SetParameters == NULL)
		return -11;
	if (iVMR.VBVMR_SetParametersW == NULL)
		return -12;
	if (iVMR.VBVMR_SetParameterStringA == NULL)
		return -13;
	if (iVMR.VBVMR_SetParameterStringW == NULL)
		return -14;
	if (iVMR.VBVMR_GetMidiMessage == NULL)
		return -15;

	if (iVMR.VBVMR_Output_GetDeviceNumber == NULL)
		return -30;
	if (iVMR.VBVMR_Output_GetDeviceDescA == NULL)
		return -31;
	if (iVMR.VBVMR_Output_GetDeviceDescW == NULL)
		return -32;
	if (iVMR.VBVMR_Input_GetDeviceNumber == NULL)
		return -33;
	if (iVMR.VBVMR_Input_GetDeviceDescA == NULL)
		return -34;
	if (iVMR.VBVMR_Input_GetDeviceDescW == NULL)
		return -35;

	if (iVMR.VBVMR_AudioCallbackRegister == NULL)
		return -40;
	if (iVMR.VBVMR_AudioCallbackStart == NULL)
		return -41;
	if (iVMR.VBVMR_AudioCallbackStop == NULL)
		return -42;
	if (iVMR.VBVMR_AudioCallbackUnregister == NULL)
		return -43;

	return 0;
}

static int voicemeeter_channel_count;

class vi_data : public StreamableReader<VBVMR_T_AUDIOBUFFER_TS> {
	std::string _name;
	obs_data_t *_settings;
	obs_source_t *_source;
	std::vector<int16_t> _route;
	std::vector<uint8_t> _silentBuffer;
	int _maxChannels;
	enum speaker_layout _layout;
	int _stage;

	//	enum speaker_layout {
	//		SPEAKERS_UNKNOWN,   /**< Unknown setting, fallback is stereo. */
	//		SPEAKERS_MONO,      /**< Channels: MONO */
	//		SPEAKERS_STEREO,    /**< Channels: FL, FR */
	//		SPEAKERS_2POINT1,   /**< Channels: FL, FR, LFE */
	//		SPEAKERS_4POINT0,   /**< Channels: FL, FR, FC, RC */
	//		SPEAKERS_4POINT1,   /**< Channels: FL, FR, FC, LFE, RC */
	//		SPEAKERS_5POINT1,   /**< Channels: FL, FR, FC, LFE, RL, RR */
	//		SPEAKERS_7POINT1 = 8, /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
	//	};
public:
	vi_data(obs_data_t *settings = nullptr, obs_source_t *source = nullptr)
		: _settings(settings), _source(source)
	{
		_route.reserve(MAX_AV_PLANES);
		_silentBuffer.reserve(MAX_AUDIO_SIZE);
		_stage = -1;
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			_route.push_back(-1);
		}
		for (int i = 0; i < MAX_AUDIO_SIZE; i++) {
			_silentBuffer.push_back(0);
		}
		update(settings);
	}

	~vi_data() {}

	std::string Name() { return _name; }

	std::string Name(std::string name) { return (_name = name); }

	void update(obs_data_t *settings)
	{
		if (!settings)
			return;
		_layout = (enum speaker_layout)obs_data_get_int(settings,
								"layout");
		if (_layout == SPEAKERS_UNKNOWN) {
			obs_audio_info aoi;
			obs_get_audio_info(&aoi);
			_layout = aoi.speakers;
		}
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			std::string name = "route " + std::to_string(i);
			_route[i] = (int16_t)obs_data_get_int(settings,
							      name.c_str());
		}
		int nStage = (int)obs_data_get_int(settings, "stage");
		if (_stage != nStage) {
			Disconnect();
			_stage = nStage;
			switch (_stage) {
			case voicemeeter_insert_in:
				OBSBufferInsertIn.AddListener(this);
				break;
			case voicemeeter_insert_out:
				OBSBufferInsertOut.AddListener(this);
				break;
			case voicemeeter_main:
				OBSBufferMain.AddListener(this);
				break;
			default:
				break;
			}
		}

		return;
	}

	static void fillLayouts(obs_property_t *list)
	{
		std::string name = "";
		obs_property_list_clear(list);
		obs_audio_info aoi;
		obs_get_audio_info(&aoi);
		switch (aoi.speakers) {
		case SPEAKERS_UNKNOWN:
			name = obs_module_text("Unknown");
			break;
		case SPEAKERS_MONO: /**< Channels: MONO */
			name = obs_module_text("Mono");
			break;
		case SPEAKERS_STEREO: /**< Channels: FL, FR */
			name = obs_module_text("Stereo");
			break;
		case SPEAKERS_2POINT1: /**< Channels: FL, FR, LFE */
			name = "2.1";
			break;
		case SPEAKERS_4POINT0: /**< Channels: FL, FR, FC, RC */
			name = "4.0";
			break;
		case SPEAKERS_4POINT1: /**< Channels: FL, FR, FC, LFE, RC */
			name = "4.1";
			break;
		case SPEAKERS_5POINT1: /**< Channels: FL, FR, FC, LFE, RL, RR */
			name = "5.1";
			break;
		case SPEAKERS_7POINT1: /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
			name = "7.1";
			break;
		default:
			break;
		}
		std::string outputName = obs_module_text("Output");
		outputName += " (" + name + ")";
		obs_property_list_add_int(list, outputName.c_str(), 0);
		name = "";
		for (int i = 0; i < 9; i++) {
			switch (i) {
			//case SPEAKERS_UNKNOWN:   /**< Unknown setting, fallback is stereo. */
			//	name = "None"
			case SPEAKERS_MONO: /**< Channels: MONO */
				name = obs_module_text("Mono");
				break;
			case SPEAKERS_STEREO: /**< Channels: FL, FR */
				name = obs_module_text("Stereo");
				break;
			case SPEAKERS_2POINT1: /**< Channels: FL, FR, LFE */
				name = "2.1";
				break;
			case SPEAKERS_4POINT0: /**< Channels: FL, FR, FC, RC */
				name = "4.0";
				break;
			case SPEAKERS_4POINT1: /**< Channels: FL, FR, FC, LFE, RC */
				name = "4.1";
				break;
			case SPEAKERS_5POINT1: /**< Channels: FL, FR, FC, LFE, RL, RR */
				name = "5.1";
				break;
			case SPEAKERS_7POINT1: /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
				name = "7.1";
				break;
			default:
				continue;
			}
			obs_property_list_add_int(list, name.c_str(), i);
		}
	}

	static bool channelsModified(obs_properties_t *props,
				     obs_property_t *list, obs_data_t *settings)
	{
		UNUSED_PARAMETER(props);
		obs_property_list_clear(list);
		obs_property_list_add_int(list, obs_module_text("Mute"), -1);
		int stage = (int)obs_data_get_int(settings, "stage");

		int ret = iVMR.VBVMR_GetVoicemeeterType(&vb_type);
		if (ret != 0) {
			vb_type = 0;
			return true;
		}
		int total = 0;

		int inputs = validInputs[vb_type];
		int outputs = validOutputs[vb_type];
		int mains = validMains[vb_type];

		std::string name;

		switch (stage) {
		case voicemeeter_insert_in:
			total = inputs;
			break;
		case voicemeeter_insert_out:
			total = outputs;
			break;
		case voicemeeter_main:
			total = mains;
			break;
		default:
			return true;
		};

		int i;

		for (i = 0; i < total; i++) {
			name = getChannelName(i, stage);
			obs_property_list_add_int(
				list, (std::to_string(i) + ": " + name).c_str(),
				i);
		}

		return true;
	}

	static bool stageChanged(obs_properties_t *props, obs_property_t *list,
				 obs_data_t *settings)
	{
		UNUSED_PARAMETER(list);
		obs_property_t *pn = obs_properties_first(props);
		/* single pass over properties */
		do {
			const char *name = obs_property_name(pn);
			if (strncmp("route ", name, 6) == 0) {
				//obs_property_list_clear(pn);
				channelsModified(props, pn, settings);
			}
		} while (obs_property_next(&pn));

		return true;
	}

	static bool layoutChanged(obs_properties_t *props, obs_property_t *list,
				  obs_data_t *settings)
	{
		enum speaker_layout layout =
			(enum speaker_layout)obs_data_get_int(
				settings, obs_property_name(list));
		if (layout == SPEAKERS_UNKNOWN) {
			obs_audio_info aoi;
			obs_get_audio_info(&aoi);
			layout = aoi.speakers;
		}
		int channels = get_audio_channels(layout);

		obs_property_t *pn = obs_properties_first(props);
		/* single pass over properties */
		int i = 0;
		do {
			const char *name = obs_property_name(pn);
			if (strncmp("route ", name, 6) == 0) {
				std::string in = (name + 6);
				i = std::stoi(in);
				obs_property_set_visible(pn, i < channels);
			}
		} while (obs_property_next(&pn));

		return true;
	}

	obs_properties_t *get_properties()
	{
		obs_properties_t *props = obs_properties_create();
		obs_property_t *prop = nullptr;
		obs_property_t *stageProperty = obs_properties_add_list(
			props, "stage", obs_module_text("Stage"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(stageProperty, "None", -1);
		obs_property_list_add_int(
			stageProperty,
			obs_module_text("Voicemeeter Insert (input)"), 0);
		obs_property_list_add_int(
			stageProperty,
			obs_module_text("Voicemeeter Insert (output)"), 1);
		obs_property_list_add_int(
			stageProperty, obs_module_text("Voicemeeter Main"), 2);
		obs_property_set_modified_callback(stageProperty, stageChanged);

		obs_property_t *layoutProperty = obs_properties_add_list(
			props, "layout", obs_module_text("Speaker Layout"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_set_modified_callback(layoutProperty,
						   layoutChanged);
		fillLayouts(layoutProperty);
		for (int i = 0; i < MAX_AV_PLANES; i++) {
			prop = obs_properties_add_list(
				props, ("route " + std::to_string(i)).c_str(),
				obs_module_text(
					("Route." + std::to_string(i)).c_str()),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_set_visible(
				prop, i < (int)get_audio_channels(_layout));
		}
		return props;
	}
	/* Sends audio data to OBS */
	void Read(const VBVMR_T_AUDIOBUFFER_TS *buf)
	{
		struct obs_source_audio out;
		out.timestamp = buf->ts;

		int limit;
		switch (vb_type) {
		case voicemeeter_potato:
		case voicemeeter_banana:
		case voicemeeter_normal:
			break;
		default:
			return;
		}

		switch (_stage) {
		case voicemeeter_insert_in:
			limit = validInputs[vb_type];
			break;
		case voicemeeter_insert_out:
			limit = validOutputs[vb_type];
			break;
		case voicemeeter_main:
			limit = validOutputs[vb_type];
			break;
		default:
			return;
		}

		_maxChannels =
			min((MAX_AV_PLANES), (get_audio_channels(_layout)));
		for (int i = 0; i < _maxChannels; i++) {
			if (_route[i] >= 0 && _route[i] < limit) {
				out.data[i] = (const uint8_t *)buf->data
						      .audiobuffer_r[_route[i]];
			} else {
				if (buf->data.audiobuffer_nbs * 4 >
				    _silentBuffer.size()) {
					_silentBuffer.reserve(
						buf->data.audiobuffer_nbs * 4);
					do {
						_silentBuffer.push_back(0);
					} while (buf->data.audiobuffer_nbs * 4 >
						 _silentBuffer.size());
				}
				out.data[i] =
					(const uint8_t *)_silentBuffer.data();
			}
		}

		out.samples_per_sec = buf->data.audiobuffer_sr;
		out.frames = buf->data.audiobuffer_nbs;
		out.format = AUDIO_FORMAT_FLOAT_PLANAR;
		out.speakers = _layout;

		obs_source_output_audio(_source, &out);
	}
};

static void *vi_create(obs_data_t *settings, obs_source_t *source)
{
	vi_data *data = new vi_data(settings, source);
	return data;
}

static void vi_destroy(void *vptr)
{
	vi_data *data = static_cast<vi_data *>(vptr);
	data->Disconnect();
	delete data;
}

static void vi_update(void *vptr, obs_data_t *settings)
{
	vi_data *data = static_cast<vi_data *>(vptr);
	data->update(settings);
}

static const char *vi_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Voicemeeter");
}

static obs_properties_t *vi_get_properties(void *vptr)
{
	vi_data *data = static_cast<vi_data *>(vptr);
	return data->get_properties();
}

static void vi_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "stage", -1);
	/*Mute by default*/
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		std::string name = "route " + std::to_string(i);
		obs_data_set_default_int(settings, name.c_str(), -1);
	}
}

bool obs_module_load(void)
{
	int ret = InitializeDLLInterfaces();
	if (ret != 0) {
		blog(LOG_INFO, ".dll failed to be initalized");
		return false;
	}

	//make data structures for properties window
	makeMainLayout(0);
	makeMainLayout(voicemeeter_normal);
	makeMainLayout(voicemeeter_banana);
	makeMainLayout(voicemeeter_potato);

	makeMap(0);
	makeMap(voicemeeter_normal);
	makeMap(voicemeeter_banana);
	makeMap(voicemeeter_potato);

	QMainWindow *main_window =
		(QMainWindow *)obs_frontend_get_main_window();

	QMenu *vb_menu = nullptr;
	QAction *vb_start = nullptr;
	QAction *vb_restart = nullptr;
	if (main_window) {
		QString vm = "Voicemeeter";
		QString restart = "Restart Audio Engine";
		vb_menu = (QMenu *)main_window->menuBar()->addMenu(vm);
		vb_restart = vb_menu->addAction(restart);
		QObject::connect(vb_restart, &QAction::triggered, []() {
			vm_stop();
			vm_start();
		});
		QString start = "Start";
		vb_start = vb_menu->addAction(start);
		QObject::connect(vb_start, &QAction::triggered, [vb_start, vb_restart]() {
			vm_login();
			vm_info();
			vm_deviceinfo();
			vm_register();
			int ret = vm_start();
			
			if (vb_start)
				vb_start->setEnabled(ret != 0);
			if (vb_restart)
				vb_restart->setEnabled(ret == 0);
		});
	}

	ret = vm_login();
	if (ret == 0) {
		//logged in
		//get info about the voicemeeter application running
		vm_info();
		vm_deviceinfo();
		vm_register();
		vm_start();
		vb_restart->setEnabled(true);
		vb_start->setEnabled(false);
	} else if (ret == 1) {
		//logged in but no application
		ret = vm_launch();
		if (ret == 0) {
			//get info about the voicemeeter application running
			vm_info();
			vm_deviceinfo();
			vm_register();
			vm_start();
			vb_restart->setEnabled(true);
			vb_start->setEnabled(false);
		} else if (vb_menu) {
			vb_start->setEnabled(true);
			vb_restart->setEnabled(false);
		}
	} else if (vb_menu) {
		vb_start->setEnabled(true);
		vb_restart->setEnabled(false);
	}

	struct obs_source_info voicemeeter_input_capture = {0};
	voicemeeter_input_capture.id = "voicemeeter_input_capture";
	voicemeeter_input_capture.type = OBS_SOURCE_TYPE_INPUT;
	voicemeeter_input_capture.output_flags = OBS_SOURCE_AUDIO;
	voicemeeter_input_capture.create = vi_create;
	voicemeeter_input_capture.destroy = vi_destroy;
	voicemeeter_input_capture.update = vi_update;
	voicemeeter_input_capture.get_defaults = vi_get_defaults;
	voicemeeter_input_capture.get_name = vi_name;
	voicemeeter_input_capture.get_properties = vi_get_properties;

	obs_register_source(&voicemeeter_input_capture);

	return true;
}

void obs_module_unload()
{
	blog(LOG_INFO, "closing streams");
	OBSBufferInsertIn.Disconnect();
	OBSBufferInsertOut.Disconnect();
	OBSBufferMain.Disconnect();

	vm_stop();
	vm_unregister();
	vm_logout();

	auto cleanUp = [](VBVMR_T_AUDIOBUFFER_TS &buf) {
		for (int i = 0; i < buf.data.audiobuffer_nbi; i++) {
			bfree(buf.data.audiobuffer_r[i]);
			buf.data.audiobuffer_r[i] = nullptr;
		}
	};
	OBSBufferInsertIn.Clear(cleanUp);
	OBSBufferInsertOut.Clear(cleanUp);
	OBSBufferMain.Clear(cleanUp);
}
