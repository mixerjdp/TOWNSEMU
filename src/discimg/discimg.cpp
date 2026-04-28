/* LICENSE>>
Copyright 2020 Soji Yamakawa (CaptainYS, http://www.ysflight.com)

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

<< LICENSE */
#include <iostream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "discimg.h"
#include "cpputil.h"
#include <libchdr/chd.h>


static char skipBuf[4096];
static bool g_discimgTrace=(nullptr!=std::getenv("TSUGARU_DISCIMG_TRACE"));

#define DISCIMG_TRACE_LN(expr) do{ \
	if(g_discimgTrace) \
	{ \
		std::ostringstream __discimgTraceOss; \
		__discimgTraceOss << expr; \
		std::cout << "[DiscImage] " << __discimgTraceOss.str() << std::endl; \
	} \
}while(false)

static const char *DiscImgTrackTypeName(unsigned int trackType)
{
	switch(trackType)
	{
	case DiscImage::TRACK_AUDIO:
		return "AUDIO";
	case DiscImage::TRACK_MODE1_DATA:
		return "MODE1";
	case DiscImage::TRACK_MODE2_DATA:
		return "MODE2";
	default:
		return "UNKNOWN";
	}
}
static const char *DiscImgLayoutTypeName(int layoutType)
{
	switch(layoutType)
	{
	case DiscImage::LAYOUT_DATA:
		return "DATA";
	case DiscImage::LAYOUT_AUDIO:
		return "AUDIO";
	case DiscImage::LAYOUT_GAP:
		return "GAP";
	case DiscImage::LAYOUT_END:
		return "END";
	default:
		return "UNKNOWN";
	}
}
static std::string DiscImgMSFToString(const DiscImage::MinSecFrm &msf)
{
	std::ostringstream oss;
	oss << msf.min << ":" << msf.sec << ":" << msf.frm;
	return oss.str();
}

static uint32_t DiscImgChecksum32(const unsigned char *data,size_t len)
{
	uint32_t sum=0;
	for(size_t i=0; i<len; ++i)
	{
		sum=(sum<<5) | (sum>>27);
		sum^=data[i];
	}
	return sum;
}

struct DiscImage::ChdState
{
	struct ChdTrack
	{
		unsigned int trackNum=0;
		unsigned int trackType=DiscImage::TRACK_UNKNOWNTYPE;
		bool rawAudioMSBFirst=false;
		unsigned int pregap=0;
		unsigned int pregap_dv=0;
		unsigned int postgap=0;
		unsigned int sectors=0;
		unsigned int lba=0;
		uint64_t fileOffset=0;
	};

	chd_file *chd=nullptr;
	std::vector<unsigned char> hunkmem;
	uint32_t unitBytes=0;
	uint32_t unitsPerHunk=0;
	uint32_t totalUnits=0;
	int oldHunk=-1;
	std::vector<ChdTrack> tracks;

	~ChdState()
	{
		if(nullptr!=chd)
		{
			chd_close(chd);
			chd=nullptr;
		}
	}
};

static uint64_t CHDCallbackFSize(void *user_data)
{
	auto fp=static_cast<FILE *>(user_data);
	if(nullptr==fp)
	{
		return static_cast<uint64_t>(-1);
	}

#ifdef _WIN32
	auto cur=_ftelli64(fp);
#else
	auto cur=ftello(fp);
#endif
	if(0>cur)
	{
		return static_cast<uint64_t>(-1);
	}
#ifdef _WIN32
	if(0!=_fseeki64(fp,0,SEEK_END))
#else
	if(0!=fseeko(fp,0,SEEK_END))
#endif
	{
		return static_cast<uint64_t>(-1);
	}
#ifdef _WIN32
	auto end=_ftelli64(fp);
	if(0!=_fseeki64(fp,cur,SEEK_SET))
#else
	auto end=ftello(fp);
	if(0!=fseeko(fp,cur,SEEK_SET))
#endif
	{
		return static_cast<uint64_t>(-1);
	}
	if(0>end)
	{
		return static_cast<uint64_t>(-1);
	}
	return static_cast<uint64_t>(end);
}

static size_t CHDCallbackFRead(void *buffer,size_t size,size_t count,void *user_data)
{
	auto fp=static_cast<FILE *>(user_data);
	if(nullptr==fp || 0==size || 0==count)
	{
		return 0;
	}
	return fread(buffer,size,count,fp);
}

static int CHDCallbackFClose(void *user_data)
{
	auto fp=static_cast<FILE *>(user_data);
	if(nullptr!=fp)
	{
		fclose(fp);
	}
	return 0;
}

static int CHDCallbackFSeek(void *user_data,int64_t offset,int whence)
{
	auto fp=static_cast<FILE *>(user_data);
	if(nullptr==fp)
	{
		return -1;
	}
#ifdef _WIN32
	return 0==_fseeki64(fp,offset,whence) ? 0 : -1;
#else
	return 0==fseeko(fp,offset,whence) ? 0 : -1;
#endif
}

static const core_file_callbacks CHD_FILE_CALLBACKS=
{
	CHDCallbackFSize,
	CHDCallbackFRead,
	CHDCallbackFClose,
	CHDCallbackFSeek
};

static bool CHDLoadRawSector(DiscImage::ChdState &st,uint64_t rawSectorIndex,unsigned char *sector)
{
	if(nullptr==st.chd || nullptr==sector || 0==st.unitBytes || 0==st.unitsPerHunk)
	{
		return false;
	}
	if(rawSectorIndex>=st.totalUnits)
	{
		DISCIMG_TRACE_LN("CHD raw sector out of range rawIndex=" << rawSectorIndex << " totalUnits=" << st.totalUnits);
		return false;
	}

	auto hunknum=static_cast<uint32_t>(rawSectorIndex/st.unitsPerHunk);
	auto hunkofs=static_cast<uint32_t>(rawSectorIndex%st.unitsPerHunk);
	if(st.oldHunk!=static_cast<int>(hunknum))
	{
		auto err=chd_read(st.chd,hunknum,st.hunkmem.data());
		if(CHDERR_NONE!=err)
		{
			DISCIMG_TRACE_LN("CHD chd_read failed hunk=" << hunknum << " err=" << err);
			return false;
		}
		st.oldHunk=static_cast<int>(hunknum);
	}
	memcpy(sector,st.hunkmem.data()+static_cast<size_t>(hunkofs)*st.unitBytes,st.unitBytes);
	return true;
}


// Uncomment for verbose output.
// #define DEBUG_DISCIMG

/* static */ DiscImage::MinSecFrm DiscImage::MinSecFrm::Zero(void)
{
	MinSecFrm MSF;
	MSF.min=0;
	MSF.sec=0;
	MSF.frm=0;
	return MSF;
}
/* static */ DiscImage::MinSecFrm DiscImage::MinSecFrm::TwoSeconds(void)
{
	MinSecFrm MSF;
	MSF.min=0;
	MSF.sec=2;
	MSF.frm=0;
	return MSF;
}


////////////////////////////////////////////////////////////

static std::string GetExtension(const std::string &fName)
{
	auto pos = fName.rfind('.');
	if (pos == std::string::npos) {
		return "";
	}

	return fName.substr(pos);
}
static void Capitalize(std::string &str)
{
	for(auto &c : str)
	{
		if('a'<=c && c<='z')
		{
			c+=('A'-'a');
		}
	}
}

static std::string QuoteForCommandLine(const std::string &str)
{
	std::string quoted="\"";
	for(auto c : str)
	{
		if('\"'==c)
		{
			quoted+="\\\"";
		}
		else
		{
			quoted.push_back(c);
		}
	}
	quoted.push_back('\"');
	return quoted;
}

static bool CreateUniqueTempDirectory(std::filesystem::path &tempDir)
{
	auto base=std::filesystem::temp_directory_path()/ "TsugaruCHD";
	std::error_code ec;
	std::filesystem::create_directories(base,ec);

	for(int retry=0; retry<128; ++retry)
	{
		auto stamp=std::chrono::high_resolution_clock::now().time_since_epoch().count();
		auto candidate=base/(std::to_string(stamp)+"_"+std::to_string(retry));
		if(std::filesystem::create_directory(candidate,ec) && !ec)
		{
			tempDir=candidate;
			return true;
		}
	}
	return false;
}

static int RunCommandAndWait(const std::string &commandLine,const std::filesystem::path &workingDir)
{
#ifdef _WIN32
	STARTUPINFOA si{};
	PROCESS_INFORMATION pi{};
	si.cb=sizeof(si);

	std::vector<char> cmdBuf(commandLine.begin(),commandLine.end());
	cmdBuf.push_back('\0');
	std::vector<char> workBuf(workingDir.string().begin(),workingDir.string().end());
	workBuf.push_back('\0');

	if(FALSE==CreateProcessA(nullptr,cmdBuf.data(),nullptr,nullptr,FALSE,0,nullptr,workBuf.data(),&si,&pi))
	{
		return -1;
	}
	WaitForSingleObject(pi.hProcess,INFINITE);
	DWORD exitCode=1;
	GetExitCodeProcess(pi.hProcess,&exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return static_cast<int>(exitCode);
#else
	return std::system(commandLine.c_str());
#endif
}


////////////////////////////////////////////////////////////


DiscImage::DiscImage()
{
	CleanUp();
}
DiscImage::~DiscImage()
{
	CleanUp();
}
/* static */ const char *DiscImage::ErrorCodeToText(unsigned int errCode)
{
	switch(errCode)
	{
	case ERROR_NOERROR:
		return "No error";
	case ERROR_UNSUPPORTED:
		return "Unsupported file format.";
	case ERROR_CANNOT_OPEN:
		return "Cannot open image file.";
	case ERROR_NOT_YET_SUPPORTED:
		return "File format not yet supported. (I'm working on it!)";
	case ERROR_SECTOR_SIZE:
		return "Binary size is not integer multiple of the sector length.";
	case ERROR_TOO_FEW_ARGS:
		return "Too few arguments.";
	case ERROR_UNSUPPORTED_TRACK_TYPE:
		return "Unsupported track type.";
	case ERROR_SECTOR_LENGTH_NOT_GIVEN:
		return "Sector length of the data track is not given.";
	case ERROR_TRACK_INFO_WITHOTU_TRACK:
		return "Track information is given without a track.";
	case ERROR_INCOMPLETE_MSF:
		return "Incomplete MM:SS:FF.";
	case ERROR_BINARY_FILE_NOT_FOUND:
		return "Image binary file not found.";
	case ERROR_FIRST_TRACK_NOT_STARTING_AT_00_00_00:
		return "First track not starting at 00:00:00";
	case ERROR_BINARY_SIZE_NOT_SECTOR_TIMES_INTEGER:
		return "Binary size is not integer multiple of sector lengths.";
	case ERROR_NUM_MULTI_BIN_NOT_EQUAL_TO_NUM_TRACKS:
		return "Number of binary files and the number of tracks in multi-bin CUE file do not match.";
	case ERROR_MULTI_BIN_DATA_NOT_2352_PER_SEC:
		return "All tracks must use 2352 bytes/sector in multi-bin CUE file.";
	case ERROR_MDS_MEDIA_TYPE:
		return "Unsupported MDS Media Type.";
	case ERROR_MDS_MULTI_SESSION_UNSUPPORTED:
		return "MDS Multi-Session Unsupported.";
	case ERROR_MDS_MULTI_FILE_UNSUPPORTED:
		return "MDS Multi-File Unsupported.";
	case ERROR_MDS_MODE2_UNSUPPORTED:
		return "MDS Mode 2 Unsupported.";
	case ERROR_MDS_FILE_SIZE_DOES_NOT_MAKE_SENSE:
		return "MDF Binary File Size does not make sense.";
	case ERROR_MDS_UNEXPECTED_NUMBER:
		return "MDS Unexpected Number.";
	}
	return "Undefined error.";
}
void DiscImage::RemoveTemporaryImage(void)
{
	if(false==temporaryImageDir.empty())
	{
		std::error_code ec;
		std::filesystem::remove_all(temporaryImageDir,ec);
		temporaryImageDir.clear();
	}
}
void DiscImage::CleanUp(void)
{
	if(false==preserveTemporaryImageDir)
	{
		RemoveTemporaryImage();
	}

	chdState.reset();
	fileType=FILETYPE_NONE;
	totalBinLength=0;
	fName="";
	num_sectors=0;
	binaries.clear();
	tracks.clear();
	layout.clear();
	binaryCache.clear();
	preserveTemporaryImageDir=false;
}
unsigned int DiscImage::Open(const std::string &fName)
{
	auto ext=GetExtension(fName);
	Capitalize(ext);
	if(".CUE"==ext)
	{
		return OpenCUE(fName);
	}
	if(".BIN"==ext)
	{
		auto withoutExt=cpputil::RemoveExtension(fName.c_str());
		auto cue=withoutExt+".cue";
		auto CUE=withoutExt+".CUE";
		auto Cue=withoutExt+".Cue";
		if(cpputil::FileExists(cue))
		{
			return OpenCUE(cue);
		}
		if(cpputil::FileExists(CUE))
		{
			return OpenCUE(CUE);
		}
		if(cpputil::FileExists(Cue))
		{
			return OpenCUE(Cue);
		}
	}
	if(".ISO"==ext)
	{
		return OpenISO(fName);
	}
	if(".MDS"==ext)
	{
		return OpenMDS(fName);
	}
	if(".MDF"==ext)
	{
		auto withoutExt=cpputil::RemoveExtension(fName.c_str());
		auto mds=withoutExt+".mds";
		auto MDS=withoutExt+".MDS";
		auto Mds=withoutExt+".Mds";
		if(cpputil::FileExists(mds))
		{
			return OpenMDS(mds);
		}
		if(cpputil::FileExists(MDS))
		{
			return OpenMDS(MDS);
		}
		if(cpputil::FileExists(Mds))
		{
			return OpenMDS(Mds);
		}
	}
	if(".CCD"==ext)
	{
		return OpenCCD(fName);
	}
	if(".CHD"==ext)
	{
		return OpenCHD(fName);
	}
	return ERROR_UNSUPPORTED;
}

static bool ParseCHDTrackType(const char *type,unsigned int &trackType)
{
	if(nullptr==type)
	{
		return false;
	}
	if(0==strncmp(type,"AUDIO",5))
	{
		trackType=DiscImage::TRACK_AUDIO;
		return true;
	}
	if(0==strncmp(type,"MODE1",5))
	{
		trackType=DiscImage::TRACK_MODE1_DATA;
		return true;
	}
	if(0==strncmp(type,"MODE2",5))
	{
		trackType=DiscImage::TRACK_MODE2_DATA;
		return true;
	}
	return false;
}

unsigned int DiscImage::OpenCUE(const std::string &fName)
{
	DISCIMG_TRACE_LN("OpenCUE begin file=" << fName);
	std::ifstream ifp;
	ifp.open(fName);
	if(true!=ifp.is_open())
	{
		return ERROR_CANNOT_OPEN;
	}

	CleanUp();
	this->fName=fName;
	fileType=FILETYPE_CUE;

	// https://en.wikipedia.org/wiki/Cue_sheet_(computing)
	enum
	{
		CMD_FILE,
		CMD_TRACK,
		CMD_INDEX,
		CMD_PREGAP,
		CMD_POSTGAP,
	};
	std::unordered_map <std::string,int> cmdMap;
	cmdMap["FILE"]=CMD_FILE;
	cmdMap["TRACK"]=CMD_TRACK;
	cmdMap["INDEX"]=CMD_INDEX;
	cmdMap["PREGAP"]=CMD_PREGAP;
	cmdMap["POSTGAP"]=CMD_POSTGAP;

	while(true!=ifp.eof())
	{
		std::string line;
		std::getline(ifp,line);

		std::vector <std::string> argv=cpputil::Parser(line.c_str());
		if(0<argv.size())
		{
			cpputil::Capitalize(argv[0]);
			auto found=cmdMap.find(argv[0]);
			if(cmdMap.end()==found)
			{
				continue;
			}
			switch(found->second)
			{
			case CMD_FILE:
				if(2<=argv.size())
				{
					Binary bin;
					bin.fName=argv[1];
					binaries.push_back(bin);
					DISCIMG_TRACE_LN("CUE FILE raw=" << argv[1] << " binaryCount=" << binaries.size());
				}
				break;
			case CMD_TRACK:
				if(3<=argv.size())
				{
					Track t;
					cpputil::Capitalize(argv[2]);
					if("AUDIO"==argv[2])
					{
						t.trackType=TRACK_AUDIO;
						t.sectorLength=2352;
					}
					else if(true==cpputil::StrStartsWith(argv[2],"MODE1"))
					{
						t.trackType=TRACK_MODE1_DATA;
						auto sectLenStr=cpputil::StrSkip(argv[2].c_str(),"/");
						if(nullptr!=sectLenStr)
						{
							t.sectorLength=cpputil::Atoi(sectLenStr);
						}
						else
						{
							return ERROR_SECTOR_LENGTH_NOT_GIVEN;
						}
					}
					tracks.push_back(t);
					DISCIMG_TRACE_LN("CUE TRACK trackNo=" << tracks.size() << " type=" << DiscImgTrackTypeName(t.trackType) << " sectorLength=" << t.sectorLength);
				}
				else
				{
					return ERROR_TOO_FEW_ARGS;
				}
				break;
			case CMD_INDEX:
				if(0==tracks.size())
				{
					return ERROR_TRACK_INFO_WITHOTU_TRACK;
				}
				if(3<=argv.size())
				{
					auto indexType=cpputil::Atoi(argv[1].c_str());
					MinSecFrm msf;
					if(true!=StrToMSF(msf,argv[2].c_str()))
					{
						return ERROR_INCOMPLETE_MSF;
					}
					if(0==indexType)
					{
						tracks.back().index00=msf;
						DISCIMG_TRACE_LN("CUE INDEX trackNo=" << tracks.size() << " index00=" << DiscImgMSFToString(msf));
					}
					else if(1==indexType)
					{
						tracks.back().start=msf;
						DISCIMG_TRACE_LN("CUE INDEX trackNo=" << tracks.size() << " start=" << DiscImgMSFToString(msf));
					}
				}
				else
				{
					return ERROR_TOO_FEW_ARGS;
				}
				break;
			case CMD_PREGAP:
				if(0==tracks.size())
				{
					return ERROR_TRACK_INFO_WITHOTU_TRACK;
				}
				if(2<=argv.size())
				{
					MinSecFrm msf;
					if(true!=StrToMSF(msf,argv[1].c_str()))
					{
						return ERROR_INCOMPLETE_MSF;
					}
					tracks.back().preGap=msf;
					DISCIMG_TRACE_LN("CUE PREGAP trackNo=" << tracks.size() << " value=" << DiscImgMSFToString(msf));
				}
				else
				{
					return ERROR_TOO_FEW_ARGS;
				}
				break;
			case CMD_POSTGAP:
				if(0==tracks.size())
				{
					return ERROR_TRACK_INFO_WITHOTU_TRACK;
				}
				if(2<=argv.size())
				{
					MinSecFrm msf;
					if(true!=StrToMSF(msf,argv[1].c_str()))
					{
						return ERROR_INCOMPLETE_MSF;
					}
					tracks.back().postGap=msf;
					DISCIMG_TRACE_LN("CUE POSTGAP trackNo=" << tracks.size() << " value=" << DiscImgMSFToString(msf));
				}
				else
				{
					return ERROR_TOO_FEW_ARGS;
				}
				break;
			default:
				std::cout << "Unrecognized: " << line << std::endl;
				break;
			}
		}
	}
	return OpenCUEPostProcess();
}
unsigned int DiscImage::OpenCHD(const std::string &fName)
{
	return OpenCHDLibChdr(fName);
}

unsigned int DiscImage::OpenCHDLibChdr(const std::string &fName)
{
	DISCIMG_TRACE_LN("OpenCHDLibChdr begin file=" << fName);
	CleanUp();

	FILE *fp=fopen(fName.c_str(),"rb");
	if(nullptr==fp)
	{
		return ERROR_CANNOT_OPEN;
	}

	chd_file *chd=nullptr;
	auto err=chd_open_core_file_callbacks(&CHD_FILE_CALLBACKS,fp,CHD_OPEN_READ,nullptr,&chd);
	if(CHDERR_NONE!=err)
	{
		fclose(fp);
		return ERROR_CANNOT_OPEN;
	}

	auto *header=chd_get_header(chd);
	if(nullptr==header || 0==header->hunkbytes || 0==header->unitbytes || 0!=(header->hunkbytes%header->unitbytes))
	{
		DISCIMG_TRACE_LN("CHD header invalid or non-divisible hunk/unit layout");
		chd_close(chd);
		return ERROR_CANNOT_OPEN;
	}
	if(AUDIO_SECTOR_SIZE!=header->unitbytes && 2448!=header->unitbytes)
	{
		DISCIMG_TRACE_LN("CHD unit size unsupported unitbytes=" << header->unitbytes);
		chd_close(chd);
		return ERROR_CANNOT_OPEN;
	}

	DISCIMG_TRACE_LN("CHD header version=" << header->version
		<< " logicalSize=" << header->logicalbytes
		<< " hunkbytes=" << header->hunkbytes
		<< " unitbytes=" << header->unitbytes
		<< " unitcount=" << header->unitcount);

	auto state=std::make_unique<ChdState>();
	state->chd=chd;
	state->unitBytes=header->unitbytes;
	state->unitsPerHunk=header->hunkbytes/header->unitbytes;
	state->totalUnits=static_cast<uint32_t>(header->unitcount);
	state->hunkmem.resize(header->hunkbytes);
	state->oldHunk=-1;
	if(0==state->unitsPerHunk)
	{
		return ERROR_CANNOT_OPEN;
	}

	std::vector<Track> newTracks;
	std::vector<DiscImage::ChdState::ChdTrack> chdTracks;
	uint32_t plba=0;
	uint64_t fileOffset=0;
	bool anyTrack=false;
	uint32_t metaIndex=0;

	while(true)
	{
		char meta_entry[256]={};
		uint32_t meta_entry_size=0;
		int tkid=0,frames=0,pregap=0,postgap=0;
		char type[64]={},subtype[32]={},pgtype[32]={},pgsub[32]={};
		auto metaErr=chd_get_metadata(chd,CDROM_TRACK_METADATA2_TAG,metaIndex,meta_entry,sizeof(meta_entry),&meta_entry_size,nullptr,nullptr);
		if(CHDERR_NONE==metaErr)
		{
			if(8!=sscanf(meta_entry,CDROM_TRACK_METADATA2_FORMAT,&tkid,type,subtype,&frames,&pregap,pgtype,pgsub,&postgap))
			{
				DISCIMG_TRACE_LN("CHD metadata parse failed (CHT2) entry=" << meta_entry);
				chd_close(chd);
				return ERROR_CANNOT_OPEN;
			}
		}
		else
		{
			metaErr=chd_get_metadata(chd,CDROM_TRACK_METADATA_TAG,metaIndex,meta_entry,sizeof(meta_entry),&meta_entry_size,nullptr,nullptr);
			if(CHDERR_NONE!=metaErr)
			{
				break;
			}
			if(4!=sscanf(meta_entry,CDROM_TRACK_METADATA_FORMAT,&tkid,type,subtype,&frames))
			{
				DISCIMG_TRACE_LN("CHD metadata parse failed (CHT) entry=" << meta_entry);
				chd_close(chd);
				return ERROR_CANNOT_OPEN;
			}
			pregap=0;
			postgap=0;
			pgtype[0]='\0';
			pgsub[0]='\0';
		}

		if(tkid<=0 || frames<=0)
		{
			DISCIMG_TRACE_LN("CHD metadata invalid tkid=" << tkid << " frames=" << frames);
			chd_close(chd);
			return ERROR_CANNOT_OPEN;
		}
		if(0!=strncmp(subtype,"NONE",4))
		{
			DISCIMG_TRACE_LN("CHD metadata unsupported subtype=" << subtype);
			chd_close(chd);
			return ERROR_CANNOT_OPEN;
		}

		unsigned int trackType=TRACK_UNKNOWNTYPE;
		if(false==ParseCHDTrackType(type,trackType))
		{
			chd_close(chd);
			return ERROR_CANNOT_OPEN;
		}

		if(static_cast<size_t>(tkid)>newTracks.size())
		{
			newTracks.resize(tkid);
			chdTracks.resize(tkid);
		}

		DiscImage::ChdState::ChdTrack chdTrk;
		chdTrk.trackNum=tkid;
		chdTrk.trackType=trackType;
		chdTrk.rawAudioMSBFirst=(TRACK_AUDIO==trackType);
		chdTrk.pregap=(1==tkid ? 150 : ('V'==pgtype[0] ? 0 : pregap));
		chdTrk.pregap_dv=('V'==pgtype[0] ? pregap : 0);
		chdTrk.postgap=postgap;
		plba+=chdTrk.pregap+chdTrk.pregap_dv;
		chdTrk.lba=plba;
		chdTrk.sectors=frames-chdTrk.pregap_dv;
		chdTrk.fileOffset=fileOffset;
		fileOffset+=chdTrk.pregap_dv;
		fileOffset+=frames-chdTrk.pregap_dv;
		fileOffset+=chdTrk.postgap;
		fileOffset+=((frames+3)&~3)-frames;
		plba+=frames-chdTrk.pregap_dv;
		plba+=chdTrk.postgap;

		Track trk;
		trk.trackType=trackType;
		trk.sectorLength=DEFAULT_SECTOR_SIZE;
		trk.preGapSectorLength=DEFAULT_SECTOR_SIZE;
		if(1==tkid)
		{
			// Match Tsugaru's CUE interpretation for the boot track:
			// logical HSG 0 must land on the first readable sector.
			trk.start=MinSecFrm::Zero();
			trk.end=HSGtoMSF(chdTrk.sectors-1);
			trk.preGap=MinSecFrm::Zero();
		}
		else
		{
			// CHD stores the absolute disc LBA with the 150-sector lead-in offset.
			// Tsugaru's CUE logic, on the other hand, works in relative HSG space.
			// Keep the physical raw offset in chdTrk.fileOffset, but shift the
			// logical track positions so audio/data requests land on the same
			// sectors as the CUE path.
			auto logicalLba=(150<=chdTrk.lba ? chdTrk.lba-150 : 0);
			trk.start=HSGtoMSF(logicalLba);
			trk.end=HSGtoMSF(logicalLba+chdTrk.sectors-1);
			trk.preGap=HSGtoMSF(chdTrk.pregap);
		}
		trk.postGap=HSGtoMSF(chdTrk.postgap);
		trk.index00=MinSecFrm::Zero();
		trk.locationInFile=chdTrk.fileOffset;
		newTracks[tkid-1]=trk;
		chdTracks[tkid-1]=chdTrk;
		DISCIMG_TRACE_LN("CHD track " << tkid
			<< " type=" << DiscImgTrackTypeName(trackType)
			<< " frames=" << frames
			<< " pregap=" << pregap
			<< " pregap_dv=" << chdTrk.pregap_dv
			<< " postgap=" << postgap
			<< " lba=" << chdTrk.lba
			<< " sectors=" << chdTrk.sectors
			<< " fileOffset=" << chdTrk.fileOffset
			<< " start=" << DiscImgMSFToString(trk.start)
			<< " end=" << DiscImgMSFToString(trk.end));
		anyTrack=true;
		++metaIndex;
	}

	if(false==anyTrack)
	{
		chd_close(chd);
		return ERROR_CANNOT_OPEN;
	}

	// Rebuild the logical end of each track from the next track's logical start.
	// CHD metadata exposes absolute LBAs with lead-in, while Tsugaru's CUE path
	// treats track ranges in the logical HSG space used by the VM.  Deriving the
	// end boundary from the next track keeps CHD and CUE aligned at transitions.
	for(size_t i=0; i+1<newTracks.size(); ++i)
	{
		if(TRACK_UNKNOWNTYPE!=newTracks[i].trackType &&
		   TRACK_UNKNOWNTYPE!=newTracks[i+1].trackType)
		{
			auto nextLogicalLba=(150<=chdTracks[i+1].lba ? chdTracks[i+1].lba-150 : 0);
			if(0<nextLogicalLba)
			{
				newTracks[i].end=HSGtoMSF(nextLogicalLba-1);
			}
		}
	}
	if(g_discimgTrace)
	{
		for(size_t i=0; i<newTracks.size(); ++i)
		{
			DISCIMG_TRACE_LN("CHD logical track " << (i+1)
				<< " type=" << DiscImgTrackTypeName(newTracks[i].trackType)
				<< " start=" << DiscImgMSFToString(newTracks[i].start)
				<< " end=" << DiscImgMSFToString(newTracks[i].end)
				<< " locationInFile=" << newTracks[i].locationInFile);
		}
	}

	fileType=FILETYPE_CUE;
	this->fName=fName;
	num_sectors=0;
	for(const auto &trk : newTracks)
	{
		auto trackEnd=trk.end.ToHSG()+1;
		num_sectors=(num_sectors<trackEnd ? trackEnd : num_sectors);
	}
	tracks=std::move(newTracks);
	chdState=std::move(state);
	chdState->tracks=std::move(chdTracks);
	DISCIMG_TRACE_LN("Loaded CHD via libchdr unitBytes=" << chdState->unitBytes << " hunkBytes=" << header->hunkbytes << " tracks=" << tracks.size());
	for(size_t i=0; i<tracks.size(); ++i)
	{
		auto const &trk=tracks[i];
		auto const &chdTrk=chdState->tracks[i];
		DISCIMG_TRACE_LN("  FINAL TRACK " << (i+1) << " type=" << DiscImgTrackTypeName(trk.trackType)
			<< " start=" << DiscImgMSFToString(trk.start)
			<< " end=" << DiscImgMSFToString(trk.end)
			<< " preGap=" << DiscImgMSFToString(trk.preGap)
			<< " postGap=" << DiscImgMSFToString(trk.postGap)
			<< " fileOffset=" << chdTrk.fileOffset
			<< " sectors=" << chdTrk.sectors);
	}
	return ERROR_NOERROR;
}
unsigned int DiscImage::OpenCUEPostProcess(void)
{
	DISCIMG_TRACE_LN("OpenCUEPostProcess begin tracks=" << tracks.size() << " binaries=" << binaries.size() << " totalBinLength=" << totalBinLength);
	if(0==tracks.size())
	{
		return ERROR_NOERROR;
	}
	if(MinSecFrm::Zero()!=tracks.front().start ||
	   MinSecFrm::Zero()!=tracks.front().preGap ||
	   MinSecFrm::Zero()!=tracks.front().index00)
	{
		return ERROR_FIRST_TRACK_NOT_STARTING_AT_00_00_00;
	}


	if(1<binaries.size()) // Multi-bin (1)
	{
		if(binaries.size()!=tracks.size())
		{
			// The number of binaries needs to match the number of tracks.
			return ERROR_NUM_MULTI_BIN_NOT_EQUAL_TO_NUM_TRACKS;
		}
		if(2352!=tracks[0].sectorLength)
		{
			// Non-2352 bytes/sec not supported in multi-bin format.
			return ERROR_MULTI_BIN_DATA_NOT_2352_PER_SEC;
		}
		// I have absolutely no idea if I am interpreting it right.
		// Multi-bin CUE files are so inconsistent.
		for(int trk=0; trk<tracks.size(); ++trk)
		{
			// I hope it is a correct interpretation.
			binaries[trk].bytesToSkip=2352*MSFtoHSG(tracks[trk].preGap+tracks[trk].start);
		}
	}


	uint64_t binLength=0;
	for(auto &bin : binaries)
	{
		std::vector <std::string> binFileCandidate;
		{
			std::string path,file;
			cpputil::SeparatePathFile(path,file,fName);
			binFileCandidate.push_back(path+bin.fName);
		}
		{
			std::string base=cpputil::RemoveExtension(fName.c_str());
			binFileCandidate.push_back(base+".BIN");
			binFileCandidate.push_back(base+".IMG");
			binFileCandidate.push_back(base+".bin");
			binFileCandidate.push_back(base+".img");
			binFileCandidate.push_back(base+".Bin");
			binFileCandidate.push_back(base+".Img");
		}
		bin.fName="";
		for(auto fn : binFileCandidate)
		{
			auto len=cpputil::FileSize(fn);
			if(0<len)
			{
				binLength+=len;
				bin.fName=fn;
				bin.fileSize=len;
				break;
			}
		}
	}
	if(0==binLength)
	{
		return ERROR_BINARY_FILE_NOT_FOUND;
	}
	this->totalBinLength=binLength;


	if(1<binaries.size()) // Multi-bin (2)
	{
		// Information in multi-bin CUE file is very erratic and unreliable.
		// I need to re-calculate based on the binary file sizes.
		// Whoever designed .CUE data format did a poor job by having PREGAP and INDEX 00.
		// INDEX 00 shouldn't have existed at all.  So confusing.
		unsigned int startSector=0;
		uint64_t locationInFile=0;
		for(int i=0; i<tracks.size(); ++i)
		{
			uint32_t numBytes=binaries[i].fileSize;
			unsigned int numSec=numBytes/tracks[i].sectorLength;
			unsigned int numPreGapSec=binaries[i].bytesToSkip/tracks[i].sectorLength;
			tracks[i].start=HSGtoMSF(startSector+numPreGapSec);
			tracks[i].end=HSGtoMSF(startSector+numSec-numPreGapSec-1);
			tracks[i].locationInFile=locationInFile;
			binaries[i].byteOffsetInDisc=locationInFile + binaries[i].bytesToSkip;
			locationInFile+=numBytes;
			startSector+=numSec;
		}
	}

	if(1==tracks.size())
	{
		DISCIMG_TRACE_LN("CUE postprocess single-track interpretation");
		unsigned int numSec=(unsigned int)binLength/tracks[0].sectorLength;
		if(0!=(binLength%tracks[0].sectorLength))
		{
			return ERROR_BINARY_SIZE_NOT_SECTOR_TIMES_INTEGER;
		}
		--numSec;
		tracks[0].end=HSGtoMSF(numSec);
		tracks[0].locationInFile=0;
	}
	else
	{
		bool analyzed=false;
		if(true==TryAnalyzeTracksWithProbablyCorrectInterpretation())
		{
			analyzed=true;
			DISCIMG_TRACE_LN("CUE postprocess used probably-correct interpretation");
		}
		else if(true==TryAnalyzeTracksWithAbsurdCUEInterpretation())
		{
			analyzed=true;
			DISCIMG_TRACE_LN("CUE postprocess used absurd interpretation");
		}
		else if(true==TryAnalyzeTracksWithMoreReasonableCUEInterpretation())
		{
			analyzed=true;
			DISCIMG_TRACE_LN("CUE postprocess used more-reasonable interpretation");
		}
		if(true!=analyzed)
		{
			// All interpretations failed.
			return ERROR_BINARY_SIZE_NOT_SECTOR_TIMES_INTEGER;
		}
	}

	if(0<tracks.size())
	{
		num_sectors=tracks.back().end.ToHSG()+1;  // LastSectorNumber+1
	}

	MakeLayoutFromTracksAndBinaryFiles();
	DISCIMG_TRACE_LN("OpenCUEPostProcess complete num_sectors=" << num_sectors << " tracks=" << tracks.size());
	for(size_t i=0; i<tracks.size(); ++i)
	{
		auto const &trk=tracks[i];
		DISCIMG_TRACE_LN("  TRACK " << (i+1) << " type=" << DiscImgTrackTypeName(trk.trackType)
			<< " sectorLength=" << trk.sectorLength
			<< " start=" << DiscImgMSFToString(trk.start)
			<< " end=" << DiscImgMSFToString(trk.end)
			<< " preGap=" << DiscImgMSFToString(trk.preGap)
			<< " postGap=" << DiscImgMSFToString(trk.postGap)
			<< " locationInFile=" << trk.locationInFile);
	}

#ifdef DEBUG_DISCIMG
	for(auto L : layout)
	{
		std::cout << "LAYOUT ";
		switch(L.layoutType)
		{
		case LAYOUT_DATA:
			std::cout << "DATA  ";
			break;
		case LAYOUT_AUDIO:
			std::cout << "AUDIO ";
			break;
		case LAYOUT_GAP:
			std::cout << "GAP   ";
			break;
		case LAYOUT_END:
			std::cout << "END   ";
			break;
		default:
			std::cout << "????? ";
			break;
		}
		std::cout << L.startHSG << " " << L.locationInFile << " " << totalBinLength << std::endl;
	}
#endif

	return ERROR_NOERROR;
}
void DiscImage::MakeLayoutFromTracksAndBinaryFiles(void)
{
	for(long long int i=0; i<tracks.size(); ++i)
	{
		DiscLayout L;
		switch(tracks[i].trackType)
		{
		case TRACK_MODE1_DATA:
		case TRACK_MODE2_DATA:
			L.layoutType=LAYOUT_DATA;
			break;
		case TRACK_AUDIO:
			L.layoutType=LAYOUT_AUDIO;
			break;
		}
		L.numSectors=tracks[i].end.ToHSG()-tracks[i].start.ToHSG()+1;
		L.sectorLength=tracks[i].sectorLength;
		L.startHSG=tracks[i].start.ToHSG();
		L.indexToBinary=(1==binaries.size() ? 0 : i);
		if(0==i)
		{
			L.locationInFile=0;
			layout.push_back(L);
		}
		else
		{
			auto locationInFile=tracks[i].locationInFile;
			auto preGapInHSG=tracks[i].preGap.ToHSG();
			if(0<preGapInHSG)
			{
				DiscLayout preGap;
				preGap.layoutType=LAYOUT_GAP;
				preGap.sectorLength=tracks[i].preGapSectorLength;
				preGap.startHSG=tracks[i].start.ToHSG()-preGapInHSG;
				preGap.numSectors=preGapInHSG;
				preGap.locationInFile=locationInFile;
				preGap.indexToBinary=(1==binaries.size() ? 0 : i);
				layout.push_back(preGap);
				locationInFile+=tracks[i].preGapSectorLength*preGapInHSG;
			}
			L.startHSG+=preGapInHSG;
			L.locationInFile=locationInFile;
			layout.push_back(L);
		}
	}
	{
		DiscLayout L;
		L.layoutType=LAYOUT_END;
		L.sectorLength=0;
		L.numSectors=0;
		L.indexToBinary=(1==binaries.size() ? 0 : binaries.size()-1);
		if(0<layout.size())
		{
			L.startHSG=layout.back().startHSG+layout.back().numSectors;
			L.locationInFile=layout.back().locationInFile+layout.back().sectorLength*layout.back().numSectors;
		}
		else
		{
			L.startHSG=0;
			L.locationInFile=0;
		}
		layout.push_back(L);
	}
	if(g_discimgTrace)
	{
		DISCIMG_TRACE_LN("MakeLayoutFromTracksAndBinaryFiles layoutCount=" << layout.size());
		for(size_t i=0; i<layout.size(); ++i)
		{
			auto const &L=layout[i];
			DISCIMG_TRACE_LN("  LAYOUT " << i << " type=" << DiscImgLayoutTypeName(L.layoutType)
				<< " startHSG=" << L.startHSG
				<< " numSectors=" << L.numSectors
				<< " sectorLength=" << L.sectorLength
				<< " locationInFile=" << L.locationInFile
				<< " binIndex=" << L.indexToBinary);
		}
	}
}
bool DiscImage::TryAnalyzeTracksWithProbablyCorrectInterpretation(void)
{
	// Interpretation based on .CUE file written by Alcohol 52% and CD Manipulator.
	// Other image-ripping program may write different .CUE, but I don't care.

	// Looks like if keyword PREGAP exists it means that pre-gap sectors are not
	// written in the binary, but insert 2 second of silence when it plays.
	// POSTGAP is similar, but it is after the track.
	// Implication is the length of the disc is the length calculated from the binary
	// plus length specified by PREGAP and POSTGAP.

	// Looks like if the track has PREGAP, from the CD-player point of view, 
	// PREGAP+INDEX 01 is the starting time of the track.
	// Therefore, if it says INDEX 01 10:00:00, and if PREGAP 00:02:00
	// existed before or on that track, it needs to be presented as 10:02:00.

	// Need to set:
	//   preGapSectorLength
	//   start
	//   end
	//   locationInFile


	// If no PREGAP, same as TryAnalyzeTracksWithAbsurdCUEInterpretation.


	// If this interpretation works, sectors of PREGAP are not stored in the binary.
	// Therefore, pre-gap sector length does not matter.
	// But, the total sector count of PREGAP needs to be added to the number of sectors
	// calculated from the binary size.
	MinSecFrm totalPREGAP;
	totalPREGAP.Set(0,0,0);
	size_t pointerInBinary=0;
	for(size_t i=0; i<tracks.size(); ++i)
	{
		tracks[i].preGapSectorLength=0;
		totalPREGAP+=tracks[i].preGap;

		tracks[i].locationInFile=pointerInBinary;
		if(i+1<tracks.size())
		{
			auto trackLen=tracks[i+1].start-tracks[i].start;
			pointerInBinary+=(tracks[i].sectorLength*trackLen.ToHSG());
		}

		tracks[i].start+=totalPREGAP;  // Displace the track-start MSF.
		tracks[i].preGap.FromHSG(0); // Then forget about this cursed PREGAP.
	}


	// pointerInBinary should be pointing to the pointer for the last track here.
	// Then, the number of sectors of the last track must be:
	auto lastTrackNumBytes=totalBinLength-pointerInBinary;
	if(lastTrackNumBytes%tracks.back().sectorLength || 0==lastTrackNumBytes)
	{
		// Inconsistency.  Remaining bytes must be integer times last track's sector length.
		return false;
	}
	auto lastTrackNumSec=lastTrackNumBytes/tracks.back().sectorLength;

	tracks.back().end.FromHSG(lastTrackNumSec-1);
	tracks.back().end+=tracks.back().start;

	for(size_t i=0; i+1<tracks.size(); ++i)
	{
		tracks[i].end=tracks[i+1].start;
		tracks[i].end.Decrement();
	}

	return true;
}
bool DiscImage::TryAnalyzeTracksWithAbsurdCUEInterpretation(void)
{
	for(long long int i=0; i<(int)tracks.size(); ++i)
	{
		// Why I say this interpretation is absurd.
		// Why pre-gap sector length of this sector needs to be the sector length of the previous track?
		tracks[i].preGapSectorLength=(0==i ? 0 : tracks[i-1].sectorLength);
	}

	long long int prevTrackSizeInBytes=0;
	for(long long int i=0; i<(int)tracks.size(); ++i)
	{
		long long int trackLength=0,gapLength=0;
		if(i+1<tracks.size())
		{
			auto endMSF=tracks[i+1].start-tracks[i+1].preGap;
			auto endHSG=endMSF.ToHSG();
			tracks[i].end=HSGtoMSF(endHSG-1);
			trackLength=(endHSG-tracks[i].start.ToHSG())*tracks[i].sectorLength;
		}
		if(1<=i)
		{
			tracks[i].locationInFile=tracks[i-1].locationInFile+prevTrackSizeInBytes;
			auto preGap=tracks[i].preGap.ToHSG();
			gapLength=preGap*tracks[i].preGapSectorLength;
		}
		prevTrackSizeInBytes=trackLength+gapLength;
	}

	auto lastTrackBytes=totalBinLength-tracks.back().locationInFile;
	if(0!=(lastTrackBytes%tracks.back().sectorLength))
	{
		return false;
	}
	auto lastTrackNumSec=(totalBinLength-tracks.back().locationInFile)/tracks.back().sectorLength;
	auto lastSectorHSG=MSFtoHSG(tracks.back().start)+lastTrackNumSec-1;
	tracks.back().end=HSGtoMSF((unsigned int)lastSectorHSG);

	return true;
}
bool DiscImage::TryAnalyzeTracksWithMoreReasonableCUEInterpretation(void)
{
	// More straight-forward interpretation, but I am not sure if it is correct.
	for(long long int i=0; i<(int)tracks.size(); ++i)
	{
		tracks[i].preGapSectorLength=2352;
	}

	for(long long int i=0; i<tracks.size(); ++i)
	{
		if(i+1<tracks.size())
		{
			tracks[i].end=HSGtoMSF(tracks[i+1].start.ToHSG()-tracks[i+1].preGap.ToHSG()-1);
		}
		tracks[i].locationInFile=(tracks[i].start.ToHSG()-tracks[i].preGap.ToHSG())*2352;
	}

	auto lastTrackBytes=totalBinLength-tracks.back().locationInFile;
	if(0!=(lastTrackBytes%tracks.back().sectorLength))
	{
		return false;
	}
	auto lastTrackNumSec=(totalBinLength-tracks.back().locationInFile)/tracks.back().sectorLength;
	auto lastSectorHSG=MSFtoHSG(tracks.back().start)+lastTrackNumSec-1;
	tracks.back().end=HSGtoMSF((unsigned int)lastSectorHSG);

	return true;
}

unsigned int DiscImage::OpenISO(const std::string &fName)
{
	std::ifstream ifp;
	ifp.open(fName,std::ios::binary);
	if(true!=ifp.is_open())
	{
		return ERROR_CANNOT_OPEN;
	}

	auto begin=ifp.tellg();
	ifp.seekg(0,std::ios::end);
	auto end=ifp.tellg();

	auto fSize=end-begin;

	fSize&=(~2047); // Some image-generation tools adds extra bytes that need to be ignored.
	// if(0!=fSize%2048)
	// {
	// 	return ERROR_SECTOR_SIZE;
	// }

	CleanUp();


	fileType=FILETYPE_ISO;
	this->fName=fName;

	Binary bin;
	bin.fName=fName;
	binaries.push_back(bin);

	num_sectors=(unsigned int)(fSize/2048);

	tracks.resize(1);
	tracks[0].trackType=TRACK_MODE1_DATA;
	tracks[0].sectorLength=2048;
	tracks[0].start=MinSecFrm::Zero();
	tracks[0].end=HSGtoMSF(num_sectors);
	tracks[0].postGap=MinSecFrm::Zero();

	return ERROR_NOERROR;
}

unsigned int DiscImage::OpenMDS(const std::string &fName)
{
	std::ifstream ifp(fName,std::ios::binary);
	if(true==ifp.is_open())
	{
		uint64_t mdfSize=0;
		std::string mdfFName;
		std::string fNameBase=cpputil::RemoveExtension(fName.c_str());
		const std::string MDFCandidate[]=
		{
			fNameBase+".MDF",
			fNameBase+".mdf",
			fNameBase+".Mdf"
		};
		for(auto fn : MDFCandidate)
		{
			auto s=cpputil::FileSize(fn);
			if(0!=s)
			{
				mdfSize=s;
				mdfFName=fn;
				break;
			}
		}
		if(0==mdfSize)
		{
			return ERROR_BINARY_FILE_NOT_FOUND;
		}



		unsigned char MDSHeaderBytes[0x58];
		ifp.read((char *)MDSHeaderBytes,0x58);

		std::string FileID;
		for(int i=0; i<16; ++i)
		{
			FileID.push_back(MDSHeaderBytes[i]);
		}
		unsigned int mediaType=cpputil::GetWord(MDSHeaderBytes+0x12);
		unsigned int nSessions=cpputil::GetWord(MDSHeaderBytes+0x14);
		uint32_t sessionOffset=cpputil::GetDword(MDSHeaderBytes+0x50);
		std::cout << "Media Type:" << mediaType << std::endl;
		std::cout << "Number of Sessions:" << nSessions << std::endl;
		std::cout << "Session Offset:" << cpputil::Itox(sessionOffset) << std::endl;
		if(0!=mediaType && 1!=mediaType && 2!=mediaType)
		{
			return ERROR_MDS_MEDIA_TYPE;
		}
		if(1!=nSessions)
		{
			return ERROR_MDS_MULTI_SESSION_UNSUPPORTED;
		}



		ifp.seekg(sessionOffset,ifp.beg);
		unsigned char MDSSessionBytes[0x18];
		ifp.read((char *)MDSSessionBytes,0x18);
		uint32_t startSec=cpputil::GetDword(MDSSessionBytes);
		uint32_t sectorCount=cpputil::GetDword(MDSSessionBytes+0x04);
		// According to https://problemkaputt.de/psx-spx.htm#cdromdiskimagesmdsmdfalcohol120,
		// MDSSessionBytes+0x04 is the end sector.  I suspect it is a sector count including the first 2 seconds (150 sectors).

		// The interpretation of the sector count is in the dark again.
		// Does it include the first two seconds where TOC and other information is stored?
		// Does it include PreGap sectors?
		// There is absolutely no answer, and there even is a chance that nobody has ever
		// defined it.
		// In BIN/CUE, it was impossible to know the sector count other than dividing the
		// binary size by the sector length.
		// However, in MDS/MDF it is more reasonable to assume that the sector count is
		// the start sector of the last track plus the number of sectors of the last track.
		// The number of sectors of the last track can be calculated by subtracting
		// .MDF file size minus start position in file of the last track.
		// This start position in file was critically missing in BIN/CUE format.

		unsigned int sessionID=cpputil::GetWord(MDSSessionBytes+0x08);
		unsigned int nDataBlocks=MDSSessionBytes[0x0A];
		unsigned int nLeadInInfo=MDSSessionBytes[0x0B];
		unsigned int firstTrack=cpputil::GetWord(MDSSessionBytes+0x0C);
		unsigned int lastTrack=cpputil::GetWord(MDSSessionBytes+0x0E);
		unsigned int dataBlockOffset=cpputil::GetDword(MDSSessionBytes+0x14);
		std::cout << "Start Sector:" << cpputil::Itox(startSec) << std::endl;
		std::cout << "Sector Count:" << sectorCount << std::endl;
		std::cout << "Session ID:" << sessionID << std::endl;
		std::cout << "Num Data Blocks:" << nDataBlocks << std::endl;
		std::cout << "Num Lead In:" << nLeadInInfo << std::endl;
		std::cout << "First Track:" << firstTrack << std::endl;
		std::cout << "Last Track:" << lastTrack << std::endl;
		std::cout << "Data Block Offset:" << dataBlockOffset << std::endl;

		if(0xFFFFFFFF-149!=startSec)
		{
			// Start sector needs to be -150
			return ERROR_UNSUPPORTED;
		}



		CleanUp();
		this->fName=fName;
		fileType=FILETYPE_MDS;

		num_sectors=sectorCount-150; // This interpretation is questionable.

		bool first=true;
		uint32_t prevFileNameOffset=0;

		ifp.seekg(dataBlockOffset,ifp.beg);
		for(unsigned int i=0; i<nDataBlocks; ++i)
		{
			unsigned char dataBlockBytes[0x50];
			ifp.read((char *)dataBlockBytes,0x50);

			unsigned int trackMode=dataBlockBytes[0];
			// A9:Audio        2352 bytes/sec
			// AA:Mode1        2048 bytes/sec
			// AB:Mode2        2336 bytes/sec
			// AC:Mode2_Form1  2048 bytes/sec
			// AD:Mode2_Form2  ?
			// EC:Mode2        2448 bytes/sec
			unsigned int numSubChannels=dataBlockBytes[1]; // 8-> +60h bytes?
			unsigned int ADR=dataBlockBytes[2]; // What is it?
			unsigned int trackNum=dataBlockBytes[3];
			unsigned int min=dataBlockBytes[0x09]; // Non-BCD
			unsigned int sec=dataBlockBytes[0x0A];
			unsigned int frm=dataBlockBytes[0x0b];

			std::cout << std::endl;
			std::cout << "Track Mode:" << cpputil::Ubtox(trackMode) << std::endl;
			std::cout << "Num Sub Channels:" << numSubChannels << std::endl;
			std::cout << "ADR(What is it?):" << ADR << std::endl;
			std::cout << "Track:" << trackNum << std::endl;
			std::cout << "Min Sec Frm:" << min << " " << sec << " " << frm << std::endl;

			if(0xA0<=dataBlockBytes[0x04]) // Lead-In Info.  What is it?
			{
				std::cout << "Lead In Info" << std::endl;
			}
			else
			{
				uint32_t offset=cpputil::GetDword(dataBlockBytes+0x0C);
				unsigned int sectorSize=cpputil::GetWord(dataBlockBytes+0x10);
				uint32_t startSector=cpputil::GetDword(dataBlockBytes+0x24);
				uint32_t MDFOffset=cpputil::GetDword(dataBlockBytes+0x28);
				uint32_t numFileNames=cpputil::GetDword(dataBlockBytes+0x30);
				uint32_t fileNameOffset=cpputil::GetDword(dataBlockBytes+0x34);

				std::cout << "Offset:" << offset << std::endl;
				std::cout << "Sector Size:" << sectorSize << std::endl;
				std::cout << "Start Sector:" << startSector << std::endl;
				std::cout << "Offset in MDF:" << MDFOffset << std::endl;
				std::cout << "Number of File Names:" << numFileNames << std::endl;
				std::cout << "File Name Offset:" << fileNameOffset << std::endl;

				if(true==first)
				{
					prevFileNameOffset=fileNameOffset;
					first=false;
				}
				else
				{
					if(prevFileNameOffset!=fileNameOffset)
					{
						return ERROR_MDS_MULTI_FILE_UNSUPPORTED;
					}
				}

				Track trk;
				switch(trackMode)
				{
				case 0xA9:
					trk.trackType=TRACK_AUDIO;
					break;
				case 0xAA:
					trk.trackType=TRACK_MODE1_DATA;
					break;
				default:
					return ERROR_MDS_MODE2_UNSUPPORTED;
				}
				trk.sectorLength=sectorSize;
				trk.preGapSectorLength=sectorSize; // I hope BIN/CUE's absurd pre-gap sector length doesn't haunt MDS.
				trk.locationInFile=MDFOffset;
				trk.start=HSGtoMSF(startSector);
				tracks.push_back(trk);
			}
		}


		// Does the file size make sense?
		for(int t=0; t+1<tracks.size(); ++t)
		{
			uint32_t trackEndSector;
			trackEndSector=MSFtoHSG(tracks[t+1].start)-1;

			tracks[t].end=HSGtoMSF(trackEndSector);
			uint32_t trackStartSector=MSFtoHSG(tracks[t].start);
		}

		// See comments above.  It makes more sense to calculate the number of sectors
		// based on the last track location in file and the binary size.
		if(tracks.back().locationInFile<mdfSize)
		{
			unsigned int lastTrackBytes=mdfSize-tracks.back().locationInFile;
			unsigned int lastTrackNumSec=lastTrackBytes/tracks.back().sectorLength;
			num_sectors=MSFtoHSG(tracks.back().start)+lastTrackNumSec;
			tracks.back().end=HSGtoMSF(num_sectors-1);
		}
		else
		{
			return ERROR_MDS_BINARY_TOO_SHORT;
		}

		Binary bin;
		bin.fName=mdfFName;
		binaries.push_back(bin);



		MakeLayoutFromTracksAndBinaryFiles();

		return ERROR_NOERROR;
	}
	return ERROR_CANNOT_OPEN;
}

unsigned int DiscImage::OpenCCD(const std::string &fName)
{
	std::ifstream ifp(fName,std::ios::binary);
	if(true==ifp.is_open())
	{
		uint64_t binSize=0;
		std::string binFName;
		std::string fNameBase=cpputil::RemoveExtension(fName.c_str());
		const std::string binCandidate[]=
		{
			fNameBase+".img",
			fNameBase+".bin",
		};
		for(auto fn : binCandidate)
		{
			auto s=cpputil::FileSize(fn);
			if(0!=s)
			{
				binSize=s;
				binFName=fn;
				break;
			}
		}
		if(0==binSize)
		{
			return ERROR_BINARY_FILE_NOT_FOUND;
		}

		CleanUp();
		this->fName=fName;
		fileType=FILETYPE_CCD;

		totalBinLength=binSize;
		num_sectors=binSize/DEFAULT_SECTOR_SIZE;

		int state=0;
		while(true!=ifp.eof())
		{
			std::string str;
			std::getline(ifp,str);

			for(auto &c : str)
			{
				c=toupper(c);
			}

			size_t i;
			bool squareBracket=false;
			for(i=0; i<str.size(); ++i)
			{
				if('['==str[i])
				{
					squareBracket=true;
					break;
				}
			}

			if(true==squareBracket) // Found '['
			{
				state=0; // Tentatively change to not in [TRACK]
				for(; i<str.size(); ++i)
				{
					if(true==cpputil::StrStartsWith(str.data()+i,"TRACK"))
					{
						Track trk;
						trk.trackType=TRACK_AUDIO; // Tentative.
						trk.sectorLength=DEFAULT_SECTOR_SIZE; // Fixed?
						trk.preGapSectorLength=0; // Doesn't matter.
						trk.locationInFile=0; // Tentative
						trk.start.FromHSG(0); // Tentative.
						tracks.push_back(trk);
						state=1;
					}
				}
			}
			else
			{
				switch(state)
				{
				case 0:
					break;
				case 1:  // Reading [TRACK ?]
					{
						size_t i=0;
						int cmd=0;
						for(; i<str.size(); ++i)
						{
							if(true==cpputil::StrStartsWith(str.data()+i,"MODE"))
							{
								cmd=1;
								i+=4;
								break;
							}
							else if(true==cpputil::StrStartsWith(str.data()+i,"INDEX"))
							{
								cmd=2;
								i+=5;
								break;
							}
						}
						if(1==cmd)
						{
							for(; i<str.size(); ++i)
							{
								if('0'<=str[i] && str[i]<='9')
								{
									switch(str[i])
									{
									case '0':
										tracks.back().trackType=TRACK_AUDIO;
										break;
									case '1':
										tracks.back().trackType=TRACK_MODE1_DATA;
										break;
									case '2':
										tracks.back().trackType=TRACK_MODE2_DATA;
										break;
									}
									break;
								}
							}
						}
						else if(2==cmd)
						{
							int indexType=-1;
							for(; i<str.size(); ++i)
							{
								if('0'<=str[i] && str[i]<='9')
								{
									indexType=cpputil::Atoi(str.c_str()+i);
									break;
								}
							}
							if(1==indexType) // Index Type 1 is all I care about.
							{
								bool passedEqual=false;
								for(; i<str.size(); ++i)
								{
									if('='==str[i])
									{
										passedEqual=true;
										break;
									}
								}
								if(true==passedEqual)
								{
									for(; i<str.size(); ++i)
									{
										if('0'<=str[i] && str[i]<='9')
										{
											size_t hsg=cpputil::Atoi(str.c_str()+i);
											tracks.back().start.FromHSG(hsg);
											tracks.back().locationInFile=DEFAULT_SECTOR_SIZE*hsg;
											break;
										}
									}
								}
							}
						}
					}
					break;
				}
			}
		}

		for(size_t i=0; i+1<tracks.size(); ++i)
		{
			tracks[i].end=tracks[i+1].start;
			tracks[i].end.Decrement();
		}
		tracks.back().end.FromHSG(num_sectors-1);

		Binary bin;
		bin.fName=binFName;
		binaries.push_back(bin);

		MakeLayoutFromTracksAndBinaryFiles();

		return ERROR_NOERROR;
	}
	return ERROR_CANNOT_OPEN;
}

bool DiscImage::CacheBinary(void)
{
	if(0<binaries.size())
	{
		std::ifstream ifp;
		ifp.open(binaries[0].fName,std::ios::binary);
		if(true==ifp.is_open())
		{
			ifp.seekg(0,std::ios::end);
			auto fSize=ifp.tellg();

			ifp.seekg(0,std::ios::beg);
			binaryCache.resize(fSize);

			ifp.read((char *)binaryCache.data(),fSize);

			return true;
		}
	}
	return false;
}

unsigned int DiscImage::GetNumTracks(void) const
{
	return (unsigned int)tracks.size();
}
unsigned int DiscImage::GetNumSectors(void) const
{
	return num_sectors;
}
const std::vector <DiscImage::Track> &DiscImage::GetTracks(void) const
{
	return tracks;
}
/* static */ DiscImage::MinSecFrm DiscImage::HSGtoMSF(unsigned int HSG)
{
	MinSecFrm MSF;
	MSF.FromHSG(HSG);
	return MSF;
}
/* static */ unsigned int DiscImage::MSFtoHSG(MinSecFrm MSF)
{
	return MSF.ToHSG();
}
/* static */ unsigned int DiscImage::BinToBCD(unsigned int bin)
{
	unsigned int high=bin/10;
	unsigned int low=bin%10;
	return (high<<4)+low;
}
/* static */ unsigned int DiscImage::BCDToBin(unsigned int bin)
{
	unsigned int high=(bin>>4);
	unsigned int low=(bin&15);
	return high*10+low;
}

static int FindTrackIndexFromHSG(const std::vector <DiscImage::Track> &tracks,unsigned int HSG)
{
	for(int i=0; i<static_cast<int>(tracks.size()); ++i)
	{
		if(tracks[i].start.ToHSG()<=HSG && HSG<=tracks[i].end.ToHSG())
		{
			return i;
		}
	}
	return -1;
}

bool DiscImage::ReadCHDRawSector(uint64_t rawSectorIndex,unsigned char *sector) const
{
	if(nullptr==chdState || nullptr==sector)
	{
		return false;
	}
	return CHDLoadRawSector(*chdState,rawSectorIndex,sector);
}

std::vector <unsigned char> DiscImage::ReadSectorMODE1(unsigned int HSG,unsigned int numSec) const
{
	std::vector <unsigned char> data;

	if(nullptr!=chdState)
	{
		if(0<tracks.size())
		{
			auto trackIndex=FindTrackIndexFromHSG(tracks,HSG);
			if(0<=trackIndex && (TRACK_MODE1_DATA==tracks[trackIndex].trackType || TRACK_MODE2_DATA==tracks[trackIndex].trackType))
			{
				auto const &trk=tracks[trackIndex];
				auto const &chdTrk=chdState->tracks[trackIndex];
				if(HSG+numSec<=trk.end.ToHSG()+1)
				{
					data.assign(numSec*MODE1_BYTES_PER_SECTOR,0);
					std::vector<unsigned char> rawSector(chdState->unitBytes);
					for(unsigned int sec=0; sec<numSec; ++sec)
					{
						auto rawIndex=chdTrk.fileOffset+(HSG-trk.start.ToHSG())+sec;
						if(true==ReadCHDRawSector(rawIndex,rawSector.data()))
						{
							DISCIMG_TRACE_LN("  CHD MODE1 sector"
								<< " hsg=" << (HSG+sec)
								<< " track=" << (trackIndex+1)
								<< " rawIndex=" << rawIndex
								<< " checksum=" << cpputil::Ubtox(DiscImgChecksum32(rawSector.data(),rawSector.size())));
							memcpy(data.data()+sec*MODE1_BYTES_PER_SECTOR,rawSector.data()+16,MODE1_BYTES_PER_SECTOR);
						}
					}
				}
			}
		}
		return data;
	}

	if(0<binaries.size())
	{
		if(0==binaryCache.size())
		{
			std::ifstream ifp;
			ifp.open(binaries[0].fName,std::ios::binary);
			if(true==ifp.is_open() && 0<tracks.size() && (tracks[0].trackType==TRACK_MODE1_DATA || tracks[0].trackType==TRACK_MODE2_DATA))
			{
				if(HSG+numSec<=tracks[0].end.ToHSG()+1)
				{
					auto sectorIntoTrack=HSG-tracks[0].start.ToHSG();
					auto locationInTrack=sectorIntoTrack*tracks[0].sectorLength;

					ifp.seekg(tracks[0].locationInFile+locationInTrack,std::ios::beg);
					data.resize(numSec*MODE1_BYTES_PER_SECTOR);
					if(MODE1_BYTES_PER_SECTOR==tracks[0].sectorLength)
					{
						ifp.read((char *)data.data(),MODE1_BYTES_PER_SECTOR*numSec);
						if(g_discimgTrace)
						{
							DISCIMG_TRACE_LN("  CUE MODE1 sector"
								<< " hsg=" << HSG
								<< " track=1"
								<< " fileOffset=" << tracks[0].locationInFile
								<< " checksum=" << cpputil::Ubtox(DiscImgChecksum32(data.data(),data.size())));
						}
					}
					else
					{
							unsigned int dataPointer=0;
							for(int i=0; i<(int)numSec; ++i)
							{
								ifp.read(skipBuf,16);
								ifp.read((char *)data.data()+dataPointer,MODE1_BYTES_PER_SECTOR);
								ifp.read(skipBuf,tracks[0].sectorLength-MODE1_BYTES_PER_SECTOR-16);
								dataPointer+=MODE1_BYTES_PER_SECTOR;
						}
					}
				}
			}
		}
		else
		{
			if(0<tracks.size() && (tracks[0].trackType==TRACK_MODE1_DATA || tracks[0].trackType==TRACK_MODE2_DATA))
			{
				if(HSG+numSec<=tracks[0].end.ToHSG()+1)
				{
					auto sectorIntoTrack=HSG-tracks[0].start.ToHSG();
					auto locationInTrack=sectorIntoTrack*tracks[0].sectorLength;

					auto filePtr=tracks[0].locationInFile+locationInTrack;
					data.resize(numSec*MODE1_BYTES_PER_SECTOR);
					if(MODE1_BYTES_PER_SECTOR==tracks[0].sectorLength)
					{
						uint64_t copyLen;
						copyLen=std::min<uint64_t>(data.size(),binaryCache.size()-filePtr);
						memcpy(data.data(),binaryCache.data()+filePtr,copyLen);
					}
					else
					{
						unsigned int dataPointer=0;
						for(int i=0; i<(int)numSec && filePtr+MODE1_BYTES_PER_SECTOR<=binaryCache.size(); ++i)
						{
							filePtr+=16;
							memcpy(data.data()+dataPointer,binaryCache.data()+filePtr,MODE1_BYTES_PER_SECTOR);
							filePtr+=tracks[0].sectorLength;
							dataPointer+=MODE1_BYTES_PER_SECTOR;
						}
					}
				}
			}
		}
	}
	return data;
}

std::vector <unsigned char> DiscImage::ReadSectorRAW(unsigned int HSG,unsigned int numSec) const
{
	std::vector <unsigned char> data;

	if(nullptr!=chdState)
	{
		if(0<tracks.size())
		{
			auto trackIndex=FindTrackIndexFromHSG(tracks,HSG);
			if(0<=trackIndex && (TRACK_MODE1_DATA==tracks[trackIndex].trackType || TRACK_MODE2_DATA==tracks[trackIndex].trackType))
			{
				auto const &trk=tracks[trackIndex];
				auto const &chdTrk=chdState->tracks[trackIndex];
				if(HSG+numSec<=trk.end.ToHSG()+1)
				{
					data.assign(numSec*RAW_BYTES_PER_SECTOR,0);
					std::vector<unsigned char> rawSector(chdState->unitBytes);
					for(unsigned int sec=0; sec<numSec; ++sec)
					{
						auto rawIndex=chdTrk.fileOffset+(HSG-trk.start.ToHSG())+sec;
						if(true==ReadCHDRawSector(rawIndex,rawSector.data()))
						{
							auto dst=data.data()+sec*RAW_BYTES_PER_SECTOR;
							memcpy(dst,rawSector.data()+12,RAW_BYTES_PER_SECTOR);
						}
					}
				}
			}
		}
		return data;
	}

	if(0<binaries.size())
	{
		std::ifstream ifp;
		ifp.open(binaries[0].fName,std::ios::binary);
		if(true==ifp.is_open() && 0<tracks.size() && (tracks[0].trackType==TRACK_MODE1_DATA || tracks[0].trackType==TRACK_MODE2_DATA))
		{
			if(HSG+numSec<=tracks[0].end.ToHSG()+1)
			{
				auto sectorIntoTrack=HSG-tracks[0].start.ToHSG();
				auto locationInTrack=sectorIntoTrack*tracks[0].sectorLength;

				ifp.seekg(tracks[0].locationInFile+locationInTrack,std::ios::beg);
				data.resize(numSec*RAW_BYTES_PER_SECTOR);
				if(MODE1_BYTES_PER_SECTOR==tracks[0].sectorLength)
				{
					for(auto &d : data) // Sorry, I don't know how to calculate first four bytes and last 288 bytes.
					{
						d=0;
					}
					unsigned int dataPointer=0;
					for(int i=0; i<(int)numSec; ++i)
					{
						ifp.read((char *)data.data()+4+dataPointer,MODE1_BYTES_PER_SECTOR);
						dataPointer+=RAW_BYTES_PER_SECTOR;
					}
				}
				else if(RAW_BYTES_PER_SECTOR<=tracks[0].sectorLength)
				{
					unsigned int dataPointer=0;
					for(int i=0; i<(int)numSec; ++i)
					{
						ifp.read(skipBuf,12);
						ifp.read((char *)data.data()+dataPointer,RAW_BYTES_PER_SECTOR);
						ifp.read(skipBuf,tracks[0].sectorLength-RAW_BYTES_PER_SECTOR-12);
						dataPointer+=RAW_BYTES_PER_SECTOR;
					}
				}
				else
				{
					for(auto &d : data) // Sorry, I don't know how to calculate first four bytes and last 288 bytes.
					{
						d=0;
					}
				}
			}
		}
	}
	return data;
}

std::vector <unsigned char> DiscImage::ReadSectorMODE2(unsigned int HSG,unsigned int numSec) const
{
	std::vector <unsigned char> data;

	if(nullptr!=chdState)
	{
		if(0<tracks.size())
		{
			auto trackIndex=FindTrackIndexFromHSG(tracks,HSG);
			if(0<=trackIndex && (TRACK_MODE1_DATA==tracks[trackIndex].trackType || TRACK_MODE2_DATA==tracks[trackIndex].trackType))
			{
				auto const &trk=tracks[trackIndex];
				auto const &chdTrk=chdState->tracks[trackIndex];
				if(HSG+numSec<=trk.end.ToHSG()+1)
				{
					data.assign(numSec*RAW_BYTES_PER_SECTOR,0);
					std::vector<unsigned char> rawSector(chdState->unitBytes);
					for(unsigned int sec=0; sec<numSec; ++sec)
					{
						auto rawIndex=chdTrk.fileOffset+(HSG-trk.start.ToHSG())+sec;
						if(true==ReadCHDRawSector(rawIndex,rawSector.data()))
						{
							auto dst=data.data()+sec*RAW_BYTES_PER_SECTOR;
							memset(dst,0,RAW_BYTES_PER_SECTOR);
							memcpy(dst+4,rawSector.data()+16,MODE2_BYTES_PER_SECTOR);
						}
					}
				}
			}
		}
		return data;
	}

	if(0<binaries.size())
	{
		std::ifstream ifp;
		ifp.open(binaries[0].fName,std::ios::binary);
		if(true==ifp.is_open() && 0<tracks.size() && (tracks[0].trackType==TRACK_MODE1_DATA || tracks[0].trackType==TRACK_MODE2_DATA))
		{
			if(HSG+numSec<=tracks[0].end.ToHSG()+1)
			{
				auto sectorIntoTrack=HSG-tracks[0].start.ToHSG();
				auto locationInTrack=sectorIntoTrack*tracks[0].sectorLength;

				ifp.seekg(tracks[0].locationInFile+locationInTrack,std::ios::beg);
				data.resize(numSec*RAW_BYTES_PER_SECTOR);
				if(MODE1_BYTES_PER_SECTOR==tracks[0].sectorLength)
				{
					for(auto &d : data) // Sorry, I don't know how to calculate first four bytes and last 288 bytes.
					{
						d=0;
					}
					unsigned int dataPointer=0;
					for(int i=0; i<(int)numSec; ++i)
					{
						ifp.read((char *)data.data()+4+dataPointer,MODE2_BYTES_PER_SECTOR);
						dataPointer+=RAW_BYTES_PER_SECTOR;
					}
				}
				else if(MODE2_BYTES_PER_SECTOR<=tracks[0].sectorLength)
				{
					unsigned int dataPointer=0;
					for(int i=0; i<(int)numSec; ++i)
					{
						ifp.read(skipBuf,16);
						ifp.read((char *)data.data()+dataPointer,MODE2_BYTES_PER_SECTOR);
						ifp.read(skipBuf,tracks[0].sectorLength-MODE2_BYTES_PER_SECTOR-16);
						dataPointer+=RAW_BYTES_PER_SECTOR;
					}
				}
				else
				{
					for(auto &d : data) // Sorry, I don't know how to calculate first four bytes and last 288 bytes.
					{
						d=0;
					}
				}
			}
		}
	}
	return data;
}

int DiscImage::GetTrackFromMSF(MinSecFrm MSF) const
{
	if(0<tracks.size())
	{
		int tLow=0,tHigh=(int)tracks.size()-1;
		while(tLow!=tHigh)
		{
			auto tMid=(tLow+tHigh)/2;
			if(MSF<tracks[tMid].start)
			{
				tHigh=tMid;
			}
			else if(tracks[tMid].end<MSF)
			{
				tLow=tMid;
			}
			else
			{
				return tMid+1;
			}
		}
		return tLow+1;
	}
	return -1;
}

std::vector <unsigned char> DiscImage::GetWave(MinSecFrm startMSF,MinSecFrm endMSF) const
{
	std::vector <unsigned char> wave;
	DISCIMG_TRACE_LN("GetWave request start=" << DiscImgMSFToString(startMSF) << " end=" << DiscImgMSFToString(endMSF) << " chd=" << (nullptr!=chdState ? "yes" : "no"));

	if(nullptr!=chdState)
	{
		if(0<tracks.size() && startMSF<endMSF)
		{
			auto startHSG=startMSF.ToHSG();
			auto endHSG=endMSF.ToHSG();
			std::vector<unsigned char> rawSector(chdState->unitBytes);
			auto copySize=std::min<unsigned int>(AUDIO_SECTOR_SIZE,chdState->unitBytes);
			int lastTrackIndex=-1;
			unsigned int segmentStartHSG=startHSG;
			unsigned int segmentEndHSG=startHSG;
			unsigned int segmentBytes=0;
			auto flushCHDSegment=[&](void)
			{
				if(segmentStartHSG<segmentEndHSG)
				{
					if(0<=lastTrackIndex)
					{
						DISCIMG_TRACE_LN("  CHD WAV segment track=" << (lastTrackIndex+1)
							<< " type=" << DiscImgTrackTypeName(tracks[lastTrackIndex].trackType)
							<< " startHSG=" << segmentStartHSG
							<< " endHSG=" << (segmentEndHSG-1)
							<< " sectors=" << (segmentEndHSG-segmentStartHSG)
							<< " bytes=" << segmentBytes);
					}
					else
					{
						DISCIMG_TRACE_LN("  CHD WAV segment gap"
							<< " startHSG=" << segmentStartHSG
							<< " endHSG=" << (segmentEndHSG-1)
							<< " sectors=" << (segmentEndHSG-segmentStartHSG)
							<< " bytes=" << segmentBytes);
					}
				}
				segmentStartHSG=segmentEndHSG;
				segmentBytes=0;
			};

			// Match the CUE path semantics: the end MSF is treated as exclusive.
			for(unsigned int hsg=startHSG; hsg<endHSG; ++hsg)
			{
				auto trackIndex=FindTrackIndexFromHSG(tracks,hsg);
				auto thisTrackIndex=(0<=trackIndex && TRACK_AUDIO==tracks[trackIndex].trackType ? trackIndex : -1);
				if(thisTrackIndex!=lastTrackIndex)
				{
					flushCHDSegment();
					lastTrackIndex=thisTrackIndex;
					segmentStartHSG=hsg;
				}
				if(0<=trackIndex && TRACK_AUDIO==tracks[trackIndex].trackType)
				{
					auto const &trk=tracks[trackIndex];
					auto const &chdTrk=chdState->tracks[trackIndex];
					auto rawIndex=chdTrk.fileOffset+(hsg-trk.start.ToHSG());
					if(true==ReadCHDRawSector(rawIndex,rawSector.data()))
					{
						if(true==chdTrk.rawAudioMSBFirst)
						{
							for(unsigned int i=0; i+1<copySize; i+=2)
							{
								std::swap(rawSector[i],rawSector[i+1]);
							}
						}
						auto cur=wave.size();
						wave.resize(cur+copySize);
						memcpy(wave.data()+cur,rawSector.data(),copySize);
						segmentEndHSG=hsg+1;
						segmentBytes+=copySize;
					}
				}
				else
				{
					auto cur=wave.size();
					wave.resize(cur+copySize);
					memset(wave.data()+cur,0,copySize);
					segmentEndHSG=hsg+1;
					segmentBytes+=copySize;
				}
			}
			flushCHDSegment();
		}
		DISCIMG_TRACE_LN("GetWave CHD result bytes=" << wave.size());
		return wave;
	}

	if(0<tracks.size() && startMSF<endMSF)
	{
		auto startHSG=startMSF.ToHSG();
		auto endHSG=endMSF.ToHSG();
		DISCIMG_TRACE_LN("  CUE WAV request HSG start=" << startHSG << " end=" << endHSG);

	#ifdef DEBUG_DISCIMG
		std::cout << "From " << startHSG << " To " << endHSG << " (" << endHSG-startHSG << ")" << std::endl;
	#endif

		for(int i=0; i+1<layout.size(); ++i)  // Condition i<layout.size()-1 will crash when layout.size()==0 because it is unsigned.
		{
			unsigned long long int readFrom=0,readTo=0;
			const auto layoutType=layout[i].layoutType;

			if(startHSG<=layout[i].startHSG)
			{
				if(LAYOUT_DATA==layout[i].layoutType) // I have a feeling that this condition does nothing....
				{
					wave.clear();
					return wave;
				}
				readFrom=layout[i].locationInFile;
			}
			else if(startHSG<layout[i+1].startHSG)
			{
				readFrom=layout[i].locationInFile+layout[i].sectorLength*(startHSG-layout[i].startHSG);
			}
			else
			{
				continue;
			}

			auto &bin=binaries[layout[i].indexToBinary]; // Do it before (*1)
			auto layoutSectorLength=layout[i].sectorLength; // Do it before (*1)

			if(layout[i+1].startHSG<=endHSG)
			{
				readTo=layout[i+1].locationInFile;
			}
			else
			{
				readTo=layout[i].locationInFile+layout[i].sectorLength*(endHSG-layout[i].startHSG);
				i=layout.size(); // Let it loop-out. (*1)
			}

			if(readFrom<readTo)
			{
				DISCIMG_TRACE_LN("  CUE WAV segment track=" << (i+1)
					<< " type=" << DiscImgLayoutTypeName(layoutType)
					<< " startHSG=" << layout[i].startHSG
					<< " endHSG=" << ((layout[i+1].startHSG<=endHSG ? layout[i+1].startHSG : endHSG)-1)
				<< " readFrom=" << readFrom
				<< " readTo=" << readTo
				<< " sectorLength=" << layoutSectorLength);
				if(layoutSectorLength<=AUDIO_SECTOR_SIZE)
				{
					auto readSize=(readTo-readFrom)&(~3);

				#ifdef DEBUG_DISCIMG
					std::cout << readFrom << " " << readTo << " " << readSize << " " << std::endl;
				#endif

					auto curSize=wave.size();
					wave.resize(wave.size()+readSize);
					for(auto i=curSize; i<wave.size(); ++i)
					{
						wave[i]=0;
					}

					// I thought DATA track is excluded by the above condition, but it looks to be wrong.
					// To prevent noise from the data track, it needs to be checked here.
					if(LAYOUT_AUDIO==layoutType)
					{
						std::ifstream ifp;
						ifp.open(bin.fName,std::ios::binary);
						if(ifp.is_open())
						{
							ifp.seekg(readFrom-bin.byteOffsetInDisc+bin.bytesToSkip,std::ios::beg);
							ifp.read((char *)(wave.data()+curSize),readSize);
							ifp.close();
						}
					}
				}
				else
				{
					auto readSize=readTo-readFrom;
					readSize/=layoutSectorLength;
					readSize*=AUDIO_SECTOR_SIZE;
					readSize&=(~3);

					auto curPos=wave.size();
					wave.resize(wave.size()+readSize);
					for(auto i=curPos; i<wave.size(); ++i)
					{
						wave[i]=0;
					}

					if(LAYOUT_AUDIO==layoutType)
					{
						std::ifstream ifp;
						ifp.open(bin.fName,std::ios::binary);
						if(ifp.is_open())
						{
							ifp.seekg(readFrom-bin.byteOffsetInDisc+bin.bytesToSkip,std::ios::beg);
							for(auto filePos=readFrom; filePos<readTo; filePos+=layoutSectorLength)
							{
								ifp.read((char *)(wave.data()+curPos),AUDIO_SECTOR_SIZE);
								if(AUDIO_SECTOR_SIZE<layoutSectorLength)
								{
									ifp.read(skipBuf,layoutSectorLength-AUDIO_SECTOR_SIZE);
								}
								curPos+=AUDIO_SECTOR_SIZE;
							}
							ifp.close();
						}
					}
				}
			}
		}
	}

	DISCIMG_TRACE_LN("GetWave CUE result bytes=" << wave.size());
	return wave;
}

DiscImage::TrackTime DiscImage::DiscTimeToTrackTime(MinSecFrm discMSF) const
{
	TrackTime trackTime;
	for(int i=0; i<tracks.size(); ++i)
	{
		if(tracks[i].start<=discMSF && discMSF<=tracks[i].end)
		{
			trackTime.track=i+1;
			trackTime.MSF=discMSF-tracks[i].start;
			break;
		}
	}
	return trackTime;
}

/* static */ bool DiscImage::StrToMSF(MinSecFrm &msf,const char str[])
{
	msf.min=cpputil::Atoi(str);
	str=cpputil::StrSkip(str,":");
	if(nullptr==str)
	{
		return false;
	}
	msf.sec=cpputil::Atoi(str);
	str=cpputil::StrSkip(str,":");
	if(nullptr==str)
	{
		return false;
	}
	msf.frm=cpputil::Atoi(str);
	return true;
}
