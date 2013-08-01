//
//  SharedUtil.h
//  hifi
//
//  Created by Stephen Birarda on 2/22/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//

#ifndef __hifi__SharedUtil__
#define __hifi__SharedUtil__

#include <math.h>
#include <stdint.h>

#include <QDebug>

#ifdef _WIN32
#include "Systime.h"
#else
#include <sys/time.h>
#include <unistd.h>
#endif

typedef unsigned char rgbColor[3];

static const float ZERO             = 0.0f;
static const float ONE              = 1.0f;
static const float ONE_HALF			= 0.5f;
static const float ONE_THIRD        = 0.333333f;
static const float PIE              = 3.141592f;
static const float PI_TIMES_TWO		= 3.141592f * 2.0f;
static const float PI_OVER_180      = 3.141592f / 180.0f;
static const float EPSILON          = 0.000001f;	//smallish positive number - used as margin of error for some computations
static const float SQUARE_ROOT_OF_2 = (float)sqrt(2);
static const float SQUARE_ROOT_OF_3 = (float)sqrt(3);
static const float METER            = 1.0f;
static const float DECIMETER        = 0.1f;
static const float CENTIMETER       = 0.01f;
static const float MILLIIMETER      = 0.001f;

uint64_t usecTimestamp(timeval *time);
uint64_t usecTimestampNow();

float randFloat();
int randIntInRange (int min, int max);
float randFloatInRange (float min,float max);
unsigned char randomColorValue(int minimum);
bool randomBoolean();

bool shouldDo(float desiredInterval, float deltaTime);

void outputBufferBits(unsigned char* buffer, int length, bool withNewLine = true);
void outputBits(unsigned char byte, bool withNewLine = true);
void printVoxelCode(unsigned char* voxelCode);
int numberOfOnes(unsigned char byte);
bool oneAtBit(unsigned char byte, int bitIndex);
void setAtBit(unsigned char& byte, int bitIndex);

int  getSemiNibbleAt(unsigned char& byte, int bitIndex);
void setSemiNibbleAt(unsigned char& byte, int bitIndex, int value);

bool isInEnvironment(const char* environment);

void switchToResourcesParentIfRequired();

void loadRandomIdentifier(unsigned char* identifierBuffer, int numBytes);

const char* getCmdOption(int argc, const char * argv[],const char* option);
bool cmdOptionExists(int argc, const char * argv[],const char* option);

void sharedMessageHandler(QtMsgType type, const char* message);

struct VoxelDetail {
	float x;
	float y;
	float z;
	float s;
	unsigned char red;
	unsigned char green;
	unsigned char blue;
};

unsigned char* pointToVoxel(float x, float y, float z, float s, unsigned char r = 0, unsigned char g = 0, unsigned char b = 0);
bool createVoxelEditMessage(unsigned char command, short int sequence, 
        int voxelCount, VoxelDetail* voxelDetails, unsigned char*& bufferOut, int& sizeOut);

#ifdef _WIN32
void usleep(int waitTime);
#endif

int insertIntoSortedArrays(void* value, float key, int originalIndex, 
                           void** valueArray, float* keyArray, int* originalIndexArray, 
                           int currentCount, int maxCount);

int removeFromSortedArrays(void* value, void** valueArray, float* keyArray, int* originalIndexArray, 
                           int currentCount, int maxCount);



// Helper Class for debugging
class debug {
public:                           
    static const char* valueOf(bool checkValue) { return checkValue ? "yes" : "no"; };
};

#endif /* defined(__hifi__SharedUtil__) */
