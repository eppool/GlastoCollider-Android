/*
	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/


#ifdef SC_WIN32
# include <string.h>
# define strcasecmp( s1, s2 ) stricmp( (s1), (s2) )
#endif
#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_HiddenWorld.h"
#include "SC_InterfaceTable.h"
#include "SC_AllocPool.h"
#include "SC_GraphDef.h"
#include "SC_UnitDef.h"
#include "SC_BufGen.h"
#include "SC_Node.h"
#include "SC_CoreAudio.h"
#include "SC_Group.h"
#include "SC_Errors.h"
#include <stdio.h>
#include "SC_Prototypes.h"
#include "SC_Samp.h"
#include "SC_DirUtils.h"
#ifdef SC_WIN32
# include "../../headers/server/SC_ComPort.h"
# include "SC_Win32Utils.h"
#else
# include "SC_ComPort.h"
#endif
#include "SC_StringParser.h"
#ifdef SC_WIN32
# include <direct.h>
#else
# include <sys/param.h>
#endif

#if (_POSIX_MEMLOCK - 0) >=  200112L
# include <sys/resource.h>
# include <sys/mman.h>
#endif

InterfaceTable gInterfaceTable;
PrintFunc gPrint = 0;

extern HashTable<struct UnitDef, Malloc> *gUnitDefLib;
extern HashTable<struct BufGen, Malloc> *gBufGenLib;
extern HashTable<PlugInCmd, Malloc> *gPlugInCmds;

extern "C" {
int sndfileFormatInfoFromStrings(struct SF_INFO *info,
	const char *headerFormatString, const char *sampleFormatString);
bool SendMsgToEngine(World *inWorld, FifoMsg& inMsg);
bool SendMsgFromEngine(World *inWorld, FifoMsg& inMsg);
}

bool sc_UseVectorUnit();
void sc_SetDenormalFlags();

////////////////////////////////////////////////////////////////////////////////

#if SC_LINUX

#if HAVE_CONFIG_H
# include "config.h"
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include <stdlib.h>
#include <errno.h>

#define SC_DBUG_MEMORY 0

inline void* sc_malloc(size_t size)
{
	return malloc(size);
}

void* sc_dbg_malloc(size_t size, const char* tag, int line)
{
	void* ptr = sc_malloc(size);
	fprintf(stderr, "sc_dbg_malloc [%s:%d] %p %zu\n", tag, line, ptr, size);
#if SC_MEMORY_ALIGNMENT > 1
	if (((intptr_t)ptr % SC_MEMORY_ALIGNMENT) != 0) {
		fprintf(stderr, "sc_dbg_malloc [%s:%d] %p %zu: memory alignment error\n",
				tag, line, ptr, size);
		abort();
	}
#endif
	return ptr;
}

inline void sc_free(void* ptr)
{
	free(ptr);
}

void sc_dbg_free(void* ptr, const char* tag, int line)
{
	fprintf(stderr, "sc_dbg_free [%s:%d]: %p\n", tag, line, ptr);
	free(ptr);
}

inline void* sc_zalloc(size_t n, size_t size)
{
	size *= n;
	if (size) {
		void* ptr = sc_malloc(size);
		if (ptr) {
			memset(ptr, 0, size);
			return ptr;
		}
	}
	return 0;
}

void* sc_dbg_zalloc(size_t n, size_t size, const char* tag, int line)
{
	void* ptr = sc_zalloc(n, size);
	fprintf(stderr, "sc_dbg_zalloc [%s:%d]: %p %zu %zu\n", tag, line, ptr, n, size);
	return ptr;
}

# if SC_DEBUG_MEMORY
#  define malloc(size)			sc_dbg_malloc((size), __FUNCTION__, __LINE__)
#  define free(ptr)				sc_dbg_free((ptr), __FUNCTION__, __LINE__)
#  define zalloc(n, size)		sc_dbg_zalloc((n), (size), __FUNCTION__, __LINE__)
# else
#  define malloc(size)			sc_malloc((size))
#  define free(ptr)				sc_free((ptr))
#  define zalloc(n, size)		sc_zalloc((n), (size))
# endif // SC_DEBUG_MEMORY

#else // !SC_LINUX

// replacement for calloc.
// calloc lazily zeroes memory on first touch. This is good for most purposes, but bad for realtime audio.
void *zalloc(size_t n, size_t size)
{
	size *= n;
	if (size) {
		void* ptr = malloc(size);
		if (ptr) {
			memset(ptr, 0, size);
			return ptr;
		}
	}
	return 0;
}
#endif // SC_LINUX

////////////////////////////////////////////////////////////////////////////////

void InterfaceTable_Init();
void InterfaceTable_Init()
{
	InterfaceTable *ft = &gInterfaceTable;

	ft->mSine = gSine;
	ft->mCosecant = gInvSine;
	ft->mSineSize = kSineSize;
	ft->mSineWavetable = gSineWavetable;

	ft->fPrint = &scprintf;

	ft->fRanSeed = &server_timeseed;

	ft->fNodeEnd = &Node_End;

	ft->fDefineUnit = &UnitDef_Create;
	ft->fDefineBufGen = &BufGen_Create;
	ft->fClearUnitOutputs = &Unit_ZeroOutputs;

	ft->fNRTAlloc = &malloc;
	ft->fNRTRealloc = &realloc;
	ft->fNRTFree = &free;

	ft->fRTAlloc = &World_Alloc;
	ft->fRTRealloc = &World_Realloc;
	ft->fRTFree = &World_Free;

	ft->fNodeRun = &Node_SetRun;

	ft->fSendTrigger = &Node_SendTrigger;
	ft->fSendNodeReply = &Node_SendReply;


	ft->fDefineUnitCmd = &UnitDef_AddCmd;
	ft->fDefinePlugInCmd = &PlugIn_DefineCmd;

	ft->fSendMsgFromRT = &SendMsgFromEngine;
	ft->fSendMsgToRT = &SendMsgToEngine;
#ifdef NO_LIBSNDFILE
	ft->fSndFileFormatInfoFromStrings = NULL;
#else
	ft->fSndFileFormatInfoFromStrings = &sndfileFormatInfoFromStrings;
#endif
	ft->fGetNode = &World_GetNode;
	ft->fGetGraph = &World_GetGraph;

	ft->fNRTLock = &World_NRTLock;
	ft->fNRTUnlock = &World_NRTUnlock;

	ft->mAltivecAvailable = sc_UseVectorUnit();

	ft->fGroup_DeleteAll = &Group_DeleteAll;
	ft->fDoneAction = &Unit_DoneAction;
	ft->fDoAsynchronousCommand = &PerformAsynchronousCommand;
	ft->fBufAlloc = &bufAlloc;
}

void initialize_library(const char *mUGensPluginPath);
void initializeScheduler();

static void World_LoadGraphDefs(World* world);
void World_LoadGraphDefs(World* world)
{
	GraphDef *list = 0;

	if(getenv("SC_SYNTHDEF_PATH")){
		if(world->mVerbosity > 0)
			scprintf("Loading synthdefs from path: %s\n", getenv("SC_SYNTHDEF_PATH"));
		SC_StringParser sp(getenv("SC_SYNTHDEF_PATH"), SC_STRPARSE_PATHDELIMITER);
		while (!sp.AtEnd()) {
			GraphDef *list = 0;
			char *path = const_cast<char *>(sp.NextToken());
			list = GraphDef_LoadDir(world, path, list);
			GraphDef_Define(world, list);
		}
	}else{
		char resourceDir[MAXPATHLEN];
		if(sc_IsStandAlone())
			sc_GetResourceDirectory(resourceDir, MAXPATHLEN);
		else
			sc_GetUserAppSupportDirectory(resourceDir, MAXPATHLEN);
		sc_AppendToPath(resourceDir, "synthdefs");
		if(world->mVerbosity > 0)
			scprintf("Loading synthdefs from default path: %s\n", resourceDir);
		list = GraphDef_LoadDir(world, resourceDir, list);
		GraphDef_Define(world, list);
	}

}

World* World_New(WorldOptions *inOptions)
{
#if (_POSIX_MEMLOCK - 0) >=  200112L
	if (inOptions->mMemoryLocking && inOptions->mRealTime)
	{
		bool lock_memory = false;

		rlimit limit;

		int failure = getrlimit(RLIMIT_MEMLOCK, &limit);
		if (failure)
			scprintf("getrlimit failure\n");
		else
		{
			if (limit.rlim_cur == RLIM_INFINITY and
				limit.rlim_max == RLIM_INFINITY)
				lock_memory = true;
			else
				scprintf("memory locking disabled due to resource limiting\n");

			if (lock_memory)
			{
				if (mlockall(MCL_FUTURE) != -1)
					scprintf("memory locking enabled.\n");
			}
		}
	}
#endif

	World *world = 0;

 /* try */ {
		static bool gLibInitted = false;
		if (!gLibInitted) {
			InterfaceTable_Init();
			initialize_library(inOptions->mUGensPluginPath);
			initializeScheduler();
			gLibInitted = true;
		}

		world = (World*)zalloc(1, sizeof(World));

		world->hw = (HiddenWorld*)zalloc(1, sizeof(HiddenWorld));

		world->hw->mAllocPool = new AllocPool(malloc, free, inOptions->mRealTimeMemorySize * 1024, 0);
		world->hw->mQuitProgram = new SC_Semaphore(0);

		extern Malloc gMalloc;

		HiddenWorld *hw = world->hw;
		hw->mGraphDefLib = new HashTable<struct GraphDef, Malloc>(&gMalloc, inOptions->mMaxGraphDefs, false);
		hw->mNodeLib = new IntHashTable<Node, AllocPool>(hw->mAllocPool, inOptions->mMaxNodes, false);
		hw->mUsers = (ReplyAddress*)zalloc(inOptions->mMaxLogins, sizeof(ReplyAddress));
		hw->mNumUsers = 0;
		hw->mMaxUsers = inOptions->mMaxLogins;
		hw->mHiddenID = -8;
		hw->mRecentID = -8;


		world->mNumUnits = 0;
		world->mNumGraphs = 0;
		world->mNumGroups = 0;

		world->mBufCounter = 0;
		world->mBufLength = inOptions->mBufLength;
		world->mSampleOffset = 0;
		world->mSubsampleOffset = 0.f;
		world->mNumAudioBusChannels = inOptions->mNumAudioBusChannels;
		world->mNumControlBusChannels = inOptions->mNumControlBusChannels;
		world->mNumInputs = inOptions->mNumInputBusChannels;
		world->mNumOutputs = inOptions->mNumOutputBusChannels;

		world->mVerbosity = inOptions->mVerbosity;
		world->mErrorNotification = 1;  // i.e., 0x01 | 0x02
		world->mLocalErrorNotification = 0;

		world->mNumSharedControls = inOptions->mNumSharedControls;
		world->mSharedControls = inOptions->mSharedControls;

		int numsamples = world->mBufLength * world->mNumAudioBusChannels;
		world->mAudioBus = (float*)zalloc(numsamples, sizeof(float));

		world->mControlBus = (float*)zalloc(world->mNumControlBusChannels, sizeof(float));

		world->mAudioBusTouched = (int32*)zalloc(inOptions->mNumAudioBusChannels, sizeof(int32));
		world->mControlBusTouched = (int32*)zalloc(inOptions->mNumControlBusChannels, sizeof(int32));

		world->mNumSndBufs = inOptions->mNumBuffers;
		world->mSndBufs = (SndBuf*)zalloc(world->mNumSndBufs, sizeof(SndBuf));
		world->mSndBufsNonRealTimeMirror = (SndBuf*)zalloc(world->mNumSndBufs, sizeof(SndBuf));
		world->mSndBufUpdates = (SndBufUpdates*)zalloc(world->mNumSndBufs, sizeof(SndBufUpdates));

		GroupNodeDef_Init();

		int err = Group_New(world, 0, &world->mTopGroup);
/* throw err; */

		world->mRealTime = inOptions->mRealTime;

		world->ft = &gInterfaceTable;

		world->mNumRGens = inOptions->mNumRGens;
		world->mRGen = new RGen[world->mNumRGens];
		for (uint32 i=0; i<world->mNumRGens; ++i) {
			world->mRGen[i].init(server_timeseed());
		}

		world->mNRTLock = new SC_Lock();
		world->mDriverLock = new SC_Lock();

		if (inOptions->mPassword) {
			strncpy(world->hw->mPassword, inOptions->mPassword, 31);
			world->hw->mPassword[31] = 0;
		} else {
			world->hw->mPassword[0] = 0;
		}

#ifdef SC_DARWIN
		world->hw->mInputStreamsEnabled = inOptions->mInputStreamsEnabled;
		world->hw->mOutputStreamsEnabled = inOptions->mOutputStreamsEnabled;
#endif
		world->hw->mInDeviceName = inOptions->mInDeviceName;
		world->hw->mOutDeviceName = inOptions->mOutDeviceName;
		hw->mMaxWireBufs = inOptions->mMaxWireBufs;
		hw->mWireBufSpace = 0;

		world->mRendezvous = inOptions->mRendezvous;

		world->mRestrictedPath = inOptions->mRestrictedPath;

		if(inOptions->mVerbosity >= 1) {
			scprintf("Using vector unit: %s\n", sc_UseVectorUnit() ? "yes" : "no");
		}
		sc_SetDenormalFlags();

		if (world->mRealTime) {
			hw->mAudioDriver = SC_NewAudioDriver(world);
			hw->mAudioDriver->SetPreferredHardwareBufferFrameSize(
					inOptions->mPreferredHardwareBufferFrameSize
			);
			hw->mAudioDriver->SetPreferredSampleRate(
					inOptions->mPreferredSampleRate
			);

			if (inOptions->mLoadGraphDefs) {
				World_LoadGraphDefs(world);
			}

			if (!hw->mAudioDriver->Setup()) {
				scprintf("could not initialize audio.\n");
				return 0;
			}
			if (!hw->mAudioDriver->Start()) {
				scprintf("start audio failed.\n");
				return 0;
			}
		} else {
			hw->mAudioDriver = 0;
		}
	}
	return world;
}

int World_CopySndBuf(World *world, uint32 index, SndBuf *outBuf, bool onlyIfChanged, bool &didChange)
{
	if (index > world->mNumSndBufs) return kSCErr_IndexOutOfRange;

	SndBufUpdates *updates = world->mSndBufUpdates + index;
	didChange = updates->reads != updates->writes;

	if (!onlyIfChanged || didChange)
	{

		world->mNRTLock->Lock();

		SndBuf *buf = world->mSndBufsNonRealTimeMirror + index;

		if (buf->data && buf->samples)
		{
			uint32 bufSize = buf->samples * sizeof(float);
			if (buf->samples != outBuf->samples)
			{
				free(outBuf->data);
				outBuf->data = (float*)malloc(bufSize);
			}
			memcpy(outBuf->data, buf->data, bufSize);
			outBuf->channels 	= buf->channels;
			outBuf->samples 	= buf->samples;
			outBuf->frames 		= buf->frames;
			outBuf->mask 		= buf->mask;
			outBuf->mask1 		= buf->mask1;
		}
		else
		{
			free(outBuf->data);
			outBuf->data = 0;
			outBuf->channels 	= 0;
			outBuf->samples 	= 0;
			outBuf->frames 		= 0;
			outBuf->mask 		= 0;
			outBuf->mask1 		= 0;
		}

		outBuf->samplerate 	= buf->samplerate;
		outBuf->sampledur 	= buf->sampledur;
		outBuf->coord 		= buf->coord;

		updates->reads = updates->writes;

		world->mNRTLock->Unlock();
	}

	return kSCErr_None;
}

bool nextOSCPacket(FILE *file, OSC_Packet *packet, int64& outTime)
{
	int32 msglen;
	if (!fread(&msglen, 1, sizeof(int32), file)) return true;
	// msglen is in network byte order
	msglen = OSCint((char*)&msglen);
	if (msglen > 8192)
/* throw std::runtime_error("OSC packet too long. > 8192 bytes\n"); */

	fread(packet->mData, 1, msglen, file);
	if (strcmp(packet->mData, "#bundle")!=0)
/* throw std::runtime_error("OSC packet not a bundle\n"); */

	packet->mSize = msglen;

	outTime = OSCtime(packet->mData+8);
	return false;
}

void PerformOSCBundle(World *inWorld, OSC_Packet *inPacket);

#ifndef NO_LIBSNDFILE
void World_NonRealTimeSynthesis(struct World *world, WorldOptions *inOptions)
{
	World_LoadGraphDefs(world);
	int bufLength = world->mBufLength;
	int fileBufFrames = inOptions->mPreferredHardwareBufferFrameSize;
	if (fileBufFrames <= 0) fileBufFrames = 8192;
	int bufMultiple = (fileBufFrames + bufLength - 1) / bufLength;
	fileBufFrames = bufMultiple * bufLength;

	// batch process non real time audio
	if (!inOptions->mNonRealTimeOutputFilename)
/* throw std::runtime_error("Non real time output filename is NULL.\n"); */

	SF_INFO inputFileInfo, outputFileInfo;
	float *inputFileBuf = 0;
	float *outputFileBuf = 0;
	int numInputChannels = 0;
	int numOutputChannels;

	outputFileInfo.samplerate = inOptions->mPreferredSampleRate;
	numOutputChannels = outputFileInfo.channels = world->mNumOutputs;
	sndfileFormatInfoFromStrings(&outputFileInfo,
		inOptions->mNonRealTimeOutputHeaderFormat, inOptions->mNonRealTimeOutputSampleFormat);

	world->hw->mNRTOutputFile = sf_open(inOptions->mNonRealTimeOutputFilename, SFM_WRITE, &outputFileInfo);
	if (!world->hw->mNRTOutputFile)
/* throw std::runtime_error("Couldn't open non real time output file.\n"); */

	outputFileBuf = (float*)calloc(1, world->mNumOutputs * fileBufFrames * sizeof(float));

	if (inOptions->mNonRealTimeInputFilename) {
		world->hw->mNRTInputFile = sf_open(inOptions->mNonRealTimeInputFilename, SFM_READ, &inputFileInfo);
		if (!world->hw->mNRTInputFile)
/* throw std::runtime_error("Couldn't open non real time input file.\n"); */

		inputFileBuf = (float*)calloc(1, inputFileInfo.channels * fileBufFrames * sizeof(float));

		if (world->mNumInputs != (uint32)inputFileInfo.channels)
			scprintf("WARNING: input file channels didn't match number of inputs specified in options.\n");

		numInputChannels = world->mNumInputs = inputFileInfo.channels; // force it.

		if (inputFileInfo.samplerate != (int)inOptions->mPreferredSampleRate)
			scprintf("WARNING: input file sample rate does not equal output sample rate.\n");

	} else {
		world->hw->mNRTInputFile = 0;
	}

	FILE *cmdFile;
	if (inOptions->mNonRealTimeCmdFilename) {
#ifdef SC_WIN32
		cmdFile = fopen(inOptions->mNonRealTimeCmdFilename, "rb");
#else
		cmdFile = fopen(inOptions->mNonRealTimeCmdFilename, "r");
#endif
	} else cmdFile = stdin;
	if (!cmdFile)
/* throw std::runtime_error("Couldn't open non real time command file.\n"); */

	char msgbuf[8192];
	OSC_Packet packet;
	memset(&packet, 0, sizeof(packet));
	packet.mData = msgbuf;
	packet.mIsBundle = true;
	packet.mReplyAddr.mReplyFunc = null_reply_func;

	int64 schedTime;
	if (nextOSCPacket(cmdFile, &packet, schedTime))
/* throw std::runtime_error("command file empty.\n"); */
	int64 prevTime = schedTime;

	World_SetSampleRate(world, inOptions->mPreferredSampleRate);
	World_Start(world);

	int64 oscTime = 0;
        double oscToSeconds = 1. / pow(2.,32.);
	double oscToSamples = inOptions->mPreferredSampleRate * oscToSeconds;
	int64 oscInc = (int64)((double)bufLength / oscToSamples);

	if(inOptions->mVerbosity >= 0) {
        printf("start time %g\n", schedTime * oscToSeconds);
	}

	bool run = true;
	int inBufStep = numInputChannels * bufLength;
	int outBufStep = numOutputChannels * bufLength;
	float* inputBuses    = world->mAudioBus + world->mNumOutputs * bufLength;
	float* outputBuses   = world->mAudioBus;
	int32* inputTouched  = world->mAudioBusTouched + world->mNumOutputs;
	int32* outputTouched = world->mAudioBusTouched;
	for (; run;) {
		int bufFramesCalculated = 0;
		float* inBufPos = inputFileBuf;
		float* outBufPos = outputFileBuf;

		if (world->hw->mNRTInputFile) {
			int framesRead = sf_readf_float(world->hw->mNRTInputFile, inputFileBuf, fileBufFrames);
			if (framesRead < fileBufFrames) {
				memset(inputFileBuf + framesRead * numInputChannels, 0,
					(fileBufFrames - framesRead) * numInputChannels * sizeof(float));
			}
		}

		for (int i=0; i<bufMultiple && run; ++i) {
			int bufCounter = world->mBufCounter;

			// deinterleave input to input buses
			if (inputFileBuf) {
				float *inBus = inputBuses;
				for (int j=0; j<numInputChannels; ++j, inBus += bufLength) {
					float *inFileBufPtr = inBufPos + j;
					for (int k=0; k<bufLength; ++k) {
						inBus[k] = *inFileBufPtr;
						inFileBufPtr += numInputChannels;
					}
					inputTouched[j] = bufCounter;
				}
			}

			// execute ready commands
			int64 nextTime = oscTime + oscInc;

			while (schedTime <= nextTime) {
				float diffTime = (float)(schedTime - oscTime) * oscToSamples + 0.5;
				float diffTimeFloor = floor(diffTime);
				world->mSampleOffset = (int)diffTimeFloor;
				world->mSubsampleOffset = diffTime - diffTimeFloor;

				if (world->mSampleOffset < 0) world->mSampleOffset = 0;
				else if (world->mSampleOffset >= bufLength) world->mSampleOffset = bufLength-1;


				PerformOSCBundle(world, &packet);
				if (nextOSCPacket(cmdFile, &packet, schedTime)) { run = false; break; }
	if(inOptions->mVerbosity >= 0) {
        printf("nextOSCPacket %g\n", schedTime * oscToSeconds);
	}
				if (schedTime < prevTime) {
					scprintf("ERROR: Packet time stamps out-of-order.\n");
					run = false;
					goto Bail;
				}
				prevTime = schedTime;
			}

			World_Run(world);

			// interleave output to output buffer
			float *outBus = outputBuses;
			for (int j=0; j<numOutputChannels; ++j, outBus += bufLength) {
				float *outFileBufPtr = outBufPos + j;
                                if (outputTouched[j] == bufCounter) {
                                    for (int k=0; k<bufLength; ++k) {
                                            *outFileBufPtr = outBus[k];
                                            outFileBufPtr += numOutputChannels;
                                    }
                                } else {
                                    for (int k=0; k<bufLength; ++k) {
                                            *outFileBufPtr = 0.f;
                                            outFileBufPtr += numOutputChannels;
                                    }
                                }
			}
			bufFramesCalculated += bufLength;
			inBufPos += inBufStep;
			outBufPos += outBufStep;
			world->mBufCounter++;
                        oscTime = nextTime;
		}

Bail:
		// write output
		sf_writef_float(world->hw->mNRTOutputFile, outputFileBuf, bufFramesCalculated);
	}

        if (cmdFile != stdin) fclose(cmdFile);
	sf_close(world->hw->mNRTOutputFile);
        world->hw->mNRTOutputFile = 0;

	if (world->hw->mNRTInputFile) {
            sf_close(world->hw->mNRTInputFile);
            world->hw->mNRTInputFile = 0;
        }

	World_Cleanup(world);
}
#endif   // !NO_LIBSNDFILE

int World_OpenUDP(struct World *inWorld, int inPort)
{
 /* try */ {
		new SC_UdpInPort(inWorld, inPort);
	}
	return true;
}

int World_OpenTCP(struct World *inWorld, int inPort, int inMaxConnections, int inBacklog)
{
 /* try */ {
		new SC_TcpInPort(inWorld, inPort, inMaxConnections, inBacklog);
		
	/* catch */
	}
	return true;
}

#if defined(SC_DARWIN) || defined(SC_IPHONE)
void World_OpenMachPorts(struct World *inWorld, CFStringRef localName, CFStringRef remoteName)
{
 /* try */ {
		new SC_MachMessagePort(inWorld, localName, remoteName);
	/* catch */
	}
}
#endif

void World_WaitForQuit(struct World *inWorld)
{
 /* try */ {
		inWorld->hw->mQuitProgram->Acquire();
		World_Cleanup(inWorld);
	/* catch */
	}
}

void World_SetSampleRate(World *inWorld, double inSampleRate)
{
	inWorld->mSampleRate = inSampleRate;
	Rate_Init(&inWorld->mFullRate, inSampleRate, inWorld->mBufLength);
	Rate_Init(&inWorld->mBufRate, inSampleRate / inWorld->mBufLength, 1);
}

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////

void* World_Alloc(World *inWorld, size_t inByteSize)
{
	return inWorld->hw->mAllocPool->Alloc(inByteSize);
}

void* World_Realloc(World *inWorld, void *inPtr, size_t inByteSize)
{
	return inWorld->hw->mAllocPool->Realloc(inPtr, inByteSize);
}

size_t World_TotalFree(World *inWorld)
{
	return inWorld->hw->mAllocPool->TotalFree();
}

size_t World_LargestFreeChunk(World *inWorld)
{
	return inWorld->hw->mAllocPool->LargestFreeChunk();
}

void World_Free(World *inWorld, void *inPtr)
{
	inWorld->hw->mAllocPool->Free(inPtr);
}

////////////////////////////////////////////////////////////////////////////////

int32 *GetKey(GraphDef *inGraphDef)
{
	return inGraphDef->mNodeDef.mName;
}

int32 GetHash(GraphDef *inGraphDef)
{
	return inGraphDef->mNodeDef.mHash;
}

void World_AddGraphDef(World *inWorld, GraphDef* inGraphDef)
{
	bool added = inWorld->hw->mGraphDefLib->Add(inGraphDef);
	if(!added) scprintf("ERROR: Could not add SynthDef %s.\nTry adjusting ServerOptions:maxSynthDefs or the -d cmdline flag.\n", (char*)inGraphDef->mNodeDef.mName);
	for (uint32 i=0; i<inGraphDef->mNumVariants; ++i) {
		GraphDef* var = inGraphDef->mVariants + i;
		added = inWorld->hw->mGraphDefLib->Add(var);
		if(!added) scprintf("ERROR: Could not add SynthDef %s.\nTry adjusting ServerOptions:maxSynthDefs or the -d cmdline flag.\n", (char*)var->mNodeDef.mName);
	}
}

void World_RemoveGraphDef(World *inWorld, GraphDef* inGraphDef)
{
	for (uint32 i=0; i<inGraphDef->mNumVariants; ++i) {
		GraphDef* var = inGraphDef->mVariants + i;
		inWorld->hw->mGraphDefLib->Remove(var);
	}
	inWorld->hw->mGraphDefLib->Remove(inGraphDef);
}

void World_FreeAllGraphDefs(World *inWorld)
{
	GrafDefTable* lib = inWorld->hw->mGraphDefLib;
	int size = lib->TableSize();
	for (int i=0; i<size; ++i) {
		GraphDef *def = lib->AtIndex(i);
		if (def) GraphDef_Free(def);
	}
	lib->MakeEmpty();
}

GraphDef* World_GetGraphDef(World *inWorld, int32* inKey)
{
	return inWorld->hw->mGraphDefLib->Get(inKey);
}

////////////////////////////////////////////////////////////////////////////////

int32 *GetKey(UnitDef *inUnitDef)
{
	return inUnitDef->mUnitDefName;
}

int32 GetHash(UnitDef *inUnitDef)
{
	return inUnitDef->mHash;
}

bool AddUnitDef(UnitDef* inUnitDef)
{
	return gUnitDefLib->Add(inUnitDef);
}

bool RemoveUnitDef(UnitDef* inUnitDef)
{
	return gUnitDefLib->Remove(inUnitDef);
}

UnitDef* GetUnitDef(int32* inKey)
{
	return gUnitDefLib->Get(inKey);
}

////////////////////////////////////////////////////////////////////////////////

int32 *GetKey(BufGen *inBufGen)
{
	return inBufGen->mBufGenName;
}

int32 GetHash(BufGen *inBufGen)
{
	return inBufGen->mHash;
}

bool AddBufGen(BufGen* inBufGen)
{
	return gBufGenLib->Add(inBufGen);
}

bool RemoveBufGen(BufGen* inBufGen)
{
	return gBufGenLib->Remove(inBufGen);
}

BufGen* GetBufGen(int32* inKey)
{
	return gBufGenLib->Get(inKey);
}

////////////////////////////////////////////////////////////////////////////////

int32 *GetKey(PlugInCmd *inPlugInCmd)
{
	return inPlugInCmd->mCmdName;
}

int32 GetHash(PlugInCmd *inPlugInCmd)
{
	return inPlugInCmd->mHash;
}

bool AddPlugInCmd(PlugInCmd* inPlugInCmd)
{
	return gPlugInCmds->Add(inPlugInCmd);
}

bool RemovePlugInCmd(PlugInCmd* inPlugInCmd)
{
	return gPlugInCmds->Remove(inPlugInCmd);
}

PlugInCmd* GetPlugInCmd(int32* inKey)
{
	return gPlugInCmds->Get(inKey);
}

////////////////////////////////////////////////////////////////////////////////

int32 GetKey(Node *inNode)
{
	return inNode->mID;
}

int32 GetHash(Node *inNode)
{
	return inNode->mHash;
}

bool World_AddNode(World *inWorld, Node* inNode)
{
	return inWorld->hw->mNodeLib->Add(inNode);
}

bool World_RemoveNode(World *inWorld, Node* inNode)
{
	return inWorld->hw->mNodeLib->Remove(inNode);
}

Node* World_GetNode(World *inWorld, int32 inID)
{
	if (inID == -1) inID = inWorld->hw->mRecentID;
	return inWorld->hw->mNodeLib->Get(inID);
}

Graph* World_GetGraph(World *inWorld, int32 inID)
{
	if (inID == -1) inID = inWorld->hw->mRecentID;
	Node *node = World_GetNode(inWorld, inID);
	if (!node) return 0;
	return node->mIsGroup ? 0 : (Graph*)node;
}

Group* World_GetGroup(World *inWorld, int32 inID)
{
	Node *node = World_GetNode(inWorld, inID);
	if (!node) return 0;
	return node->mIsGroup ? (Group*)node : 0;
}

////////////////////////////////////////////////////////////////////////////////

void World_Run(World *inWorld)
{
	// run top group
	Node *node = (Node*)inWorld->mTopGroup;
	(*node->mCalcFunc)(node);
}

void World_Start(World *inWorld)
{
	inWorld->mBufCounter = 0;
	for (uint32 i=0; i<inWorld->mNumAudioBusChannels; ++i) inWorld->mAudioBusTouched[i] = -1;
	for (uint32 i=0; i<inWorld->mNumControlBusChannels; ++i) inWorld->mControlBusTouched[i] = -1;

	inWorld->hw->mWireBufSpace = (float*)malloc(inWorld->hw->mMaxWireBufs * inWorld->mBufLength * sizeof(float));

	inWorld->hw->mTriggers.MakeEmpty();
	inWorld->hw->mNodeMsgs.MakeEmpty();
	inWorld->hw->mNodeEnds.MakeEmpty();
	inWorld->mRunning = true;
}

void World_Cleanup(World *world)
{
	if (!world) return;

	HiddenWorld *hw = world->hw;

	if (hw && world->mRealTime) hw->mAudioDriver->Stop();

	world->mRunning = false;

	if (world->mTopGroup) Group_DeleteAll(world->mTopGroup);

	world->mDriverLock->Lock(); // never unlock..
	if (hw) {
		free(hw->mWireBufSpace);
		delete hw->mAudioDriver;
		hw->mAudioDriver = 0;
	}
	delete world->mNRTLock;
	delete world->mDriverLock;
	World_Free(world, world->mTopGroup);

	for (uint32 i=0; i<world->mNumSndBufs; ++i) {
		SndBuf *nrtbuf = world->mSndBufsNonRealTimeMirror + i;
		SndBuf * rtbuf = world->mSndBufs + i;

		if (nrtbuf->data) free(nrtbuf->data);
		if (rtbuf->data && rtbuf->data != nrtbuf->data) free(rtbuf->data);

#ifndef NO_LIBSNDFILE
		if (nrtbuf->sndfile) sf_close(nrtbuf->sndfile);
		if (rtbuf->sndfile && rtbuf->sndfile != nrtbuf->sndfile) sf_close(rtbuf->sndfile);
#endif
	}

	free(world->mSndBufsNonRealTimeMirror);
	free(world->mSndBufs);

	free(world->mControlBusTouched);
	free(world->mAudioBusTouched);
	free(world->mControlBus);
	free(world->mAudioBus);
	delete [] world->mRGen;
	if (hw) {

#ifndef NO_LIBSNDFILE
		if (hw->mNRTInputFile) sf_close(hw->mNRTInputFile);
		if (hw->mNRTOutputFile) sf_close(hw->mNRTOutputFile);
		if (hw->mNRTCmdFile) fclose(hw->mNRTCmdFile);
#endif
		free(hw->mUsers);
		delete hw->mNodeLib;
		delete hw->mGraphDefLib;
		delete hw->mQuitProgram;
		delete hw->mAllocPool;
		free(hw);
	}
	free(world);
}


void World_NRTLock(World *world)
{
	world->mNRTLock->Lock();
}

void World_NRTUnlock(World *world)
{
	world->mNRTLock->Unlock();
}

////////////////////////////////////////////////////////////////////////////////


inline int32 BUFMASK(int32 x)
{
	return (1 << (31 - CLZ(x))) - 1;
}

SCErr bufAlloc(SndBuf* buf, int numChannels, int numFrames, double sampleRate)
{
	long numSamples = numFrames * numChannels;
	if(numSamples < 1) return kSCErr_Failed;
	buf->data = (float*)zalloc(numSamples, sizeof(float));
	if (!buf->data) return kSCErr_Failed;

	buf->channels = numChannels;
	buf->frames   = numFrames;
	buf->samples  = numSamples;
	buf->mask     = BUFMASK(numSamples); // for delay lines
	buf->mask1    = buf->mask - 1;	// for oscillators
	buf->samplerate = sampleRate;
	buf->sampledur = 1. / sampleRate;

	return kSCErr_None;
}

#ifndef NO_LIBSNDFILE
int sampleFormatFromString(const char* name);
int sampleFormatFromString(const char* name)
{
	if (!name) return SF_FORMAT_PCM_16;

	size_t len = strlen(name);
	if (len < 1) return 0;

	if (name[0] == 'u') {
		if (len < 5) return 0;
		if (name[4] == '8') return SF_FORMAT_PCM_U8; // uint8
		return 0;
	} else if (name[0] == 'i') {
		if (len < 4) return 0;
		if (name[3] == '8') return SF_FORMAT_PCM_S8;      // int8
		else if (name[3] == '1') return SF_FORMAT_PCM_16; // int16
		else if (name[3] == '2') return SF_FORMAT_PCM_24; // int24
		else if (name[3] == '3') return SF_FORMAT_PCM_32; // int32
	} else if (name[0] == 'f') {
		return SF_FORMAT_FLOAT; // float
	} else if (name[0] == 'd') {
		return SF_FORMAT_DOUBLE; // double
	} else if (name[0] == 'm' || name[0] == 'u') {
		return SF_FORMAT_ULAW; // mulaw ulaw
	} else if (name[0] == 'a') {
		return SF_FORMAT_ALAW; // alaw
	}
	return 0;
}

int headerFormatFromString(const char *name);
int headerFormatFromString(const char *name)
{
	if (!name) return SF_FORMAT_AIFF;
	if (strcasecmp(name, "AIFF")==0) return SF_FORMAT_AIFF;
	if (strcasecmp(name, "AIFC")==0) return SF_FORMAT_AIFF;
	if (strcasecmp(name, "RIFF")==0) return SF_FORMAT_WAV;
	if (strcasecmp(name, "WAVEX")==0) return SF_FORMAT_WAVEX;
	if (strcasecmp(name, "WAVE")==0) return SF_FORMAT_WAV;
	if (strcasecmp(name, "WAV" )==0) return SF_FORMAT_WAV;
	if (strcasecmp(name, "Sun" )==0) return SF_FORMAT_AU;
	if (strcasecmp(name, "IRCAM")==0) return SF_FORMAT_IRCAM;
	if (strcasecmp(name, "NeXT")==0) return SF_FORMAT_AU;
	if (strcasecmp(name, "raw")==0) return SF_FORMAT_RAW;
	if (strcasecmp(name, "MAT4")==0) return SF_FORMAT_MAT4;
	if (strcasecmp(name, "MAT5")==0) return SF_FORMAT_MAT5;
	if (strcasecmp(name, "PAF")==0) return SF_FORMAT_PAF;
	if (strcasecmp(name, "SVX")==0) return SF_FORMAT_SVX;
	if (strcasecmp(name, "NIST")==0) return SF_FORMAT_NIST;
	if (strcasecmp(name, "VOC")==0) return SF_FORMAT_VOC;
	if (strcasecmp(name, "W64")==0) return SF_FORMAT_W64;
	if (strcasecmp(name, "PVF")==0) return SF_FORMAT_PVF;
	if (strcasecmp(name, "XI")==0) return SF_FORMAT_XI;
	if (strcasecmp(name, "HTK")==0) return SF_FORMAT_HTK;
	if (strcasecmp(name, "SDS")==0) return SF_FORMAT_SDS;
	if (strcasecmp(name, "AVR")==0) return SF_FORMAT_AVR;
	if (strcasecmp(name, "SD2")==0) return SF_FORMAT_SD2;
	if (strcasecmp(name, "FLAC")==0) return SF_FORMAT_FLAC;
// TODO allow other platforms to know vorbis once libsndfile 1.0.18 is established
#if SC_DARWIN || SC_WIN32 || LIBSNDFILE_1018
	if (strcasecmp(name, "vorbis")==0) return SF_FORMAT_VORBIS;
#endif
	if (strcasecmp(name, "CAF")==0) return SF_FORMAT_CAF;
	return 0;
}

int sndfileFormatInfoFromStrings(struct SF_INFO *info, const char *headerFormatString, const char *sampleFormatString)
{
	int headerFormat = headerFormatFromString(headerFormatString);
	if (!headerFormat) return kSCErr_Failed;

	int sampleFormat = sampleFormatFromString(sampleFormatString);
	if (!sampleFormat) return kSCErr_Failed;

	info->format = (unsigned int)(headerFormat | sampleFormat);
	return kSCErr_None;
}
#endif

#include "scsynthsend.h"

void TriggerMsg::Perform()
{
	small_scpacket packet;
	packet.adds("/tr");
	packet.maketags(4);
	packet.addtag(',');
	packet.addtag('i');
	packet.addtag('i');
	packet.addtag('f');
	packet.addi(mNodeID);
	packet.addi(mTriggerID);
	packet.addf(mValue);

	ReplyAddress *users = mWorld->hw->mUsers;
	int numUsers = mWorld->hw->mNumUsers;
	for (int i=0; i<numUsers; ++i) {
		SendReply(users+i, packet.data(), packet.size());
	}
}

static void NodeReplyMsg_RTFree(FifoMsg* msg)
{
	//scprintf("NodeReplyMsg_RTFree()\n");
	World_Free(msg->mWorld, msg->mData);
}

void NodeReplyMsg::Perform()
{
	small_scpacket packet;
	packet.adds(mCmdName, mCmdNameSize);
	packet.maketags(3 + mNumArgs);
	packet.addtag(',');
	packet.addtag('i');
	packet.addi(mNodeID);
	packet.addtag('i');
	packet.addi(mID);
	for(int i=0; i<mNumArgs; ++i) {
		packet.addtag('f');
		packet.addf(mValues[i]);
	}

	ReplyAddress *users = mWorld->hw->mUsers;
	int numUsers = mWorld->hw->mNumUsers;
	for (int i=0; i<numUsers; ++i) {
		SendReply(users+i, packet.data(), packet.size());
	}

	// Free memory in realtime thread
	FifoMsg msg;
	msg.Set(mWorld, NodeReplyMsg_RTFree, 0, mRTMemory);
	AudioDriver(mWorld)->SendMsgToEngine(msg);
}


void NodeEndMsg::Perform()
{
	small_scpacket packet;
	switch (mState) {
		case kNode_Go :
			packet.adds("/n_go");
			break;
		case kNode_End :
			packet.adds("/n_end");
			break;
		case kNode_On :
			packet.adds("/n_on");
			break;
		case kNode_Off :
			packet.adds("/n_off");
			break;
		case kNode_Move :
			packet.adds("/n_move");
			break;
		case kNode_Info :
			packet.adds("/n_info");
			break;
	}
	if (mIsGroup) {
		packet.maketags(8);
		packet.addtag(',');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addi(mNodeID);
		packet.addi(mGroupID);
		packet.addi(mPrevNodeID);
		packet.addi(mNextNodeID);
		packet.addi(mIsGroup);
		packet.addi(mHeadID);
		packet.addi(mTailID);
	} else {
		packet.maketags(6);
		packet.addtag(',');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addtag('i');
		packet.addi(mNodeID);
		packet.addi(mGroupID);
		packet.addi(mPrevNodeID);
		packet.addi(mNextNodeID);
		packet.addi(mIsGroup);
	}

	ReplyAddress *users = mWorld->hw->mUsers;
	int numUsers = mWorld->hw->mNumUsers;
	for (int i=0; i<numUsers; ++i) {
		SendReply(users+i, packet.data(), packet.size());
	}
}

void DeleteGraphDefMsg::Perform()
{
	GraphDef_Free(mDef);
}

void NotifyNoArgs(World *inWorld, char *inString);
void NotifyNoArgs(World *inWorld, char *inString)
{
	small_scpacket packet;
	packet.adds(inString);

	ReplyAddress *users = inWorld->hw->mUsers;
	int numUsers = inWorld->hw->mNumUsers;
	for (int i=0; i<numUsers; ++i) {
		SendReply(users+i, packet.data(), packet.size());
	}
}


bool SendMsgToEngine(World *inWorld, FifoMsg& inMsg)
{
	return inWorld->hw->mAudioDriver->SendMsgToEngine(inMsg);
}

bool SendMsgFromEngine(World *inWorld, FifoMsg& inMsg)
{
	return inWorld->hw->mAudioDriver->SendMsgFromEngine(inMsg);
}

void SetPrintFunc(PrintFunc func)
{
	gPrint = func;
}


int scprintf(const char *fmt, ...)
{
	va_list vargs;
	va_start(vargs, fmt);

	if (gPrint) return (*gPrint)(fmt, vargs);
	else return vprintf(fmt, vargs);
}
