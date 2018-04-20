
#include "cxxopts.hpp"

#include <iostream>
#include <algorithm>
#include <string>
#include <thread>
#include <openvr.h>
#include <vrinputemulator.h>
#include <vector>

#define VECTOR_SUB(dst, lhs, rhs) \
		dst.v[0] = lhs.v[0] - rhs.v[0]; \
		dst.v[1] = lhs.v[1] - rhs.v[1]; \
		dst.v[2] = lhs.v[2] - rhs.v[2];

static vr::IVRSystem* m_VRSystem;
static vrinputemulator::VRInputEmulator inputEmulator;
static vr::HmdVector3d_t lastLeftPos;
static vr::HmdVector3d_t lastRightPos;
static vr::HmdVector3d_t offset;
static vr::HmdVector3d_t voffset;
static int currentFrame;
static vr::TrackedDevicePose_t devicePoses[vr::k_unMaxTrackedDeviceCount];
static vr::HmdMatrix34_t chaperoneMat;
static vr::HmdVector3d_t rightPos;
static vr::HmdVector3d_t leftPos;
static vr::TrackedDevicePose_t* rightPose;
static vr::TrackedDevicePose_t* leftPose;
static std::vector<uint32_t> virtualDeviceIndexes;

void updateVirtualDevices() {
	int count = inputEmulator.getVirtualDeviceCount();
	if (virtualDeviceIndexes.size() != count) {
		virtualDeviceIndexes.clear();
		for (uint32_t deviceIndex = 0; deviceIndex < vr::k_unMaxTrackedDeviceCount; deviceIndex++) {
			try {
				virtualDeviceIndexes.push_back(inputEmulator.getVirtualDeviceInfo(deviceIndex).openvrDeviceId);
			} catch (vrinputemulator::vrinputemulator_exception e) {
				//skip
			}
		}
	}
}

bool isVirtualDevice( uint32_t deviceIndex ) {
	if (virtualDeviceIndexes.empty()) { return false; }
	return std::find(virtualDeviceIndexes.begin(), virtualDeviceIndexes.end(), deviceIndex) != virtualDeviceIndexes.end();
}

void updateChaperoneMat() {
	vr::VRChaperoneSetup()->RevertWorkingCopy();
	vr::VRChaperoneSetup()->GetWorkingStandingZeroPoseToRawTrackingPose(&chaperoneMat);
}

void updateOffset(unsigned int leftButtonMask, unsigned int rightButtonMask) {
	float fSecondsSinceLastVsync;
	vr::VRSystem()->GetTimeSinceLastVsync(&fSecondsSinceLastVsync, NULL);
	float fDisplayFrequency = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
	float fFrameDuration = 1.f / fDisplayFrequency;
	float fVsyncToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);
	float fPredictedSecondsFromNow = fFrameDuration - fSecondsSinceLastVsync + fVsyncToPhotons;
	vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, fPredictedSecondsFromNow, devicePoses, vr::k_unMaxTrackedDeviceCount);

	vr::HmdVector3d_t delta;
	delta.v[0] = 0; delta.v[1] = 0; delta.v[2] = 0;
	auto leftId = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
	if (leftId != vr::k_unTrackedDeviceIndexInvalid ) {
		leftPose = devicePoses + leftId;
		if (leftPose->bPoseIsValid && leftPose->bDeviceIsConnected) {
			// Extract position info from matrix
			vr::HmdMatrix34_t* leftMat = &(leftPose->mDeviceToAbsoluteTracking);
			leftPos.v[0] = leftMat->m[0][3];
			leftPos.v[1] = leftMat->m[1][3];
			leftPos.v[2] = leftMat->m[2][3];
			vr::VRControllerState_t leftButtons;
			vr::VRSystem()->GetControllerState(leftId, &leftButtons, sizeof(vr::VRControllerState_t));
			if (leftButtons.ulButtonPressed & leftButtonMask ) {
				VECTOR_SUB(delta, leftPos, lastLeftPos);
			}
		}
	}
	auto rightId = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
	if (rightId != vr::k_unTrackedDeviceIndexInvalid ) {
		rightPose = devicePoses + rightId;
		if (rightPose->bPoseIsValid && rightPose->bDeviceIsConnected) {
			vr::HmdMatrix34_t* rightMat = &(rightPose->mDeviceToAbsoluteTracking);
			rightPos.v[0] = rightMat->m[0][3];
			rightPos.v[1] = rightMat->m[1][3];
			rightPos.v[2] = rightMat->m[2][3];
			vr::VRControllerState_t rightButtons;
			vr::VRSystem()->GetControllerState(rightId, &rightButtons, sizeof(vr::VRControllerState_t));
			if (rightButtons.ulButtonPressed & rightButtonMask ) {
				VECTOR_SUB(delta, rightPos, lastRightPos);
			}
		}
	}

	delta.v[0] = std::clamp(delta.v[0], (double)-0.1f, (double)0.1f);
	delta.v[1] = std::clamp(delta.v[1], (double)-0.1f, (double)0.1f);
	delta.v[2] = std::clamp(delta.v[2], (double)-0.1f, (double)0.1f);

	// prepare for the new positions to be offset by delta
	if (leftId != vr::k_unTrackedDeviceIndexInvalid) {
		if (leftPose->bPoseIsValid && leftPose->bDeviceIsConnected) {
			VECTOR_SUB(lastLeftPos, leftPos, delta);
		}
	}

	if (rightId != vr::k_unTrackedDeviceIndexInvalid) {
		if (rightPose->bPoseIsValid && rightPose->bDeviceIsConnected) {
			VECTOR_SUB(lastRightPos, rightPos, delta);
		}
	}

	// Transform the controller delta into world space
	delta.v[0] = chaperoneMat.m[0][0] * delta.v[0] + chaperoneMat.m[0][1] * delta.v[1] + chaperoneMat.m[0][2] * delta.v[2];
	delta.v[1] = chaperoneMat.m[1][0] * delta.v[0] + chaperoneMat.m[1][1] * delta.v[1] + chaperoneMat.m[1][2] * delta.v[2];
	delta.v[2] = chaperoneMat.m[2][0] * delta.v[0] + chaperoneMat.m[2][1] * delta.v[1] + chaperoneMat.m[2][2] * delta.v[2];

	VECTOR_SUB(offset, offset, delta);

	voffset.v[0] = offset.v[0]/2.f;
	voffset.v[1] = offset.v[1]/2.f;
	voffset.v[2] = offset.v[2]/2.f;
}

void Move() {
	for (uint32_t deviceIndex = 0; deviceIndex < vr::k_unMaxTrackedDeviceCount; deviceIndex++) {
		if (!vr::VRSystem()->IsTrackedDeviceConnected(deviceIndex)) {
			continue;
		}
		inputEmulator.enableDeviceOffsets(deviceIndex, true, false);
		// Virtual devices need to be moved half as much, don't ask me why
		if ( isVirtualDevice(deviceIndex) ) {
			inputEmulator.setWorldFromDriverTranslationOffset(deviceIndex, voffset, false);
		} else {
			inputEmulator.setWorldFromDriverTranslationOffset(deviceIndex, offset, false);
		}
	}
}

int main( int argc, const char** argv ) {
	cxxopts::Options options("PlayspaceMover", "Lets you grab your playspace and move it.");
	options.add_options()
		("l,leftButtonMask", "Specifies the buttons that trigger the playspace grab. (Example: 128 = X for oculus, 2 = Menu button for vive)", cxxopts::value<unsigned int>()->default_value("130"))
		("r,rightButtonMask", "Specifies the buttons that trigger the playspace grab. (Example: 128 = A for oculus, 2 = Menu button for vive)", cxxopts::value<unsigned int>()->default_value("130"))
		;
	auto result = options.parse(argc, argv);

	// Initialize stuff
	vr::EVRInitError error = vr::VRInitError_Compositor_Failed;
	std::cout << "Looking for SteamVR...";
	while (error != vr::VRInitError_None) {
		m_VRSystem = vr::VR_Init(&error, vr::VRApplication_Background);
		if (error != vr::VRInitError_None) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
	std::cout << "Success!\n";
	std::cout << "Looking for VR Input Emulator...";
	while (true) {
		try {
			inputEmulator.connect();
			break;
		}
		catch (vrinputemulator::vrinputemulator_connectionerror e) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}
	}
	std::cout << "Success!\n";

	std::cout << "Grabbing Chaperone data (You may need to set up your chaperone boundries again if this gets stuck)...";
	vr::VRChaperoneSetup()->RevertWorkingCopy();
	while (vr::VRChaperone()->GetCalibrationState() != vr::ChaperoneCalibrationState_OK) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		vr::VRChaperoneSetup()->RevertWorkingCopy();
	}
	vr::VRChaperoneSetup()->GetWorkingStandingZeroPoseToRawTrackingPose(&chaperoneMat);
	std::cout << "Success!\n";

	lastLeftPos.v[0] = 0; lastLeftPos.v[1] = 0; lastLeftPos.v[2] = 0;
	lastRightPos.v[0] = 0; lastRightPos.v[1] = 0; lastRightPos.v[2] = 0;
	offset.v[0] = 0; offset.v[1] = 0; offset.v[2] = 0;
	voffset.v[0] = 0; voffset.v[1] = 0; voffset.v[2] = 0;

	// Main loop
	bool running = true;
	while (running) {
		if (vr::VRCompositor() != NULL) {
			vr::Compositor_FrameTiming t;
			bool hasFrame = vr::VRCompositor()->GetFrameTiming(&t, 0);
			if (hasFrame && currentFrame != t.m_nFrameIndex) {
				currentFrame = t.m_nFrameIndex;
				updateVirtualDevices();
				updateChaperoneMat();
				updateOffset(result["leftButtonMask"].as<unsigned int>(), result["rightButtonMask"].as<unsigned int>());
				Move();
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
		}
	}
    return 0;
}