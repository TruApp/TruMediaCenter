/*
 *      Copyright (C) 2016 Christian Browet
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

// http://developer.android.com/reference/android/media/MediaCodec.html
//
// Android MediaCodec class can be used to access low-level media codec,
// i.e. encoder/decoder components. (android.media.MediaCodec). Requires
// SDK16+ which is 4.1 Jellybean and above.
//

#include "DVDAudioCodecAndroidMediaCodec.h"

#include "DVDCodecs/DVDCodecs.h"
#include "utils/log.h"
#include "android/jni/ByteBuffer.h"
#include "android/jni/MediaCodec.h"
#include "android/jni/MediaCrypto.h"
#include "android/jni/MediaFormat.h"
#include "android/jni/MediaCodecList.h"
#include "android/jni/MediaCodecInfo.h"
#include "android/activity/AndroidFeatures.h"
#include "android/jni/Surface.h"

#include "utils/StringUtils.h"

#include <cassert>

//#define DEBUG_VERBOSE 1

static bool IsBlacklisted(const std::string &name)
{
  static const char *blacklisted_decoders[] = {
    NULL
  };
  for (const char **ptr = blacklisted_decoders; *ptr; ptr++)
  {
    if (!strnicmp(*ptr, name.c_str(), strlen(*ptr)))
      return true;
  }
  return false;
}

static const AEChannel KnownChannels[] = { AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_SL, AE_CH_SR, AE_CH_BL, AE_CH_BR, AE_CH_BC, AE_CH_BLOC, AE_CH_BROC, AE_CH_NULL };

/****************************/

CDVDAudioCodecAndroidMediaCodec::CDVDAudioCodecAndroidMediaCodec()
: m_formatname("mediacodec")
, m_opened(false)
, m_buffer    (NULL)
, m_bufferSize(0)
, m_bufferUsed(0)
, m_samplerate(0)
, m_channels(0)
{
  memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));
}

CDVDAudioCodecAndroidMediaCodec::~CDVDAudioCodecAndroidMediaCodec()
{
  Dispose();
}

bool CDVDAudioCodecAndroidMediaCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  m_hints = hints;

  CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec::Open codec(%d), profile(%d), tag(%d), extrasize(%d)", hints.codec, hints.profile, hints.codec_tag, hints.extrasize);

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM:
      if (!m_hints.extrasize)
      {
        //TODO Support adts
        return false;
      }
      m_mime = "audio/mp4a-latm";
      m_formatname = "amc-aac";
      break;

    case AV_CODEC_ID_MP2:
      m_mime = "audio/mpeg-L2";
      m_formatname = "amc-mp2";
      break;

    case AV_CODEC_ID_MP3:
      m_mime = "audio/mpeg";
      m_formatname = "amc-mp3";
      break;

    case AV_CODEC_ID_VORBIS:
      m_mime = "audio/vorbis";
      m_formatname = "amc-ogg";

      //TODO
      return false;

      break;

    case AV_CODEC_ID_WMAPRO:
      m_mime = "audio/wmapro";
      m_formatname = "amc-wma";

      //TODO
      return false;

      break;

    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
      m_mime = "audio/x-ms-wma";
      m_formatname = "amc-wma";
      //TODO
      return false;

      break;

    case AV_CODEC_ID_AC3:
      m_mime = "audio/ac3";
      m_formatname = "amc-ac3";
      break;

    case AV_CODEC_ID_EAC3:
      m_mime = "audio/eac3";
      m_formatname = "amc-eac3";
      break;

    default:
      CLog::Log(LOGNOTICE, "CDVDAudioCodecAndroidMediaCodec:: Unknown hints.codec(%d)", hints.codec);
      return false;
      break;
  }

  m_codec = std::shared_ptr<CJNIMediaCodec>(new CJNIMediaCodec(CJNIMediaCodec::createDecoderByType(m_mime)));
  if (xbmc_jnienv()->ExceptionCheck())
  {
    // Unsupported type?
    xbmc_jnienv()->ExceptionClear();
    m_codec = NULL;
  }

  if (!m_codec)
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec:: Failed to create Android MediaCodec");
    return false;
  }

  if (!ConfigureMediaCodec())
  {
    m_codec.reset();
    return false;
  }

  CLog::Log(LOGINFO, "CDVDAudioCodecAndroidMediaCodec:: "
    "Open Android MediaCodec %s", m_codecname.c_str());

  m_opened = true;
  memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));

  return m_opened;
}

void CDVDAudioCodecAndroidMediaCodec::Dispose()
{
  if (!m_opened)
    return;

  m_opened = false;

  // release any retained demux packets
  if (m_demux_pkt.pData)
    free(m_demux_pkt.pData);

  // invalidate any inflight outputbuffers, make sure
  // m_output is empty so we do not create new ones
  m_input.clear();
  m_output.clear();

  if (m_codec)
  {
    m_codec->stop();
    m_codec->release();
    m_codec.reset();
    if (xbmc_jnienv()->ExceptionCheck())
      xbmc_jnienv()->ExceptionClear();
  }
}

int CDVDAudioCodecAndroidMediaCodec::Decode(uint8_t *pData, int iSize)
{
  int rtn = 0;

  if (!pData)
  {
    // Check if we have a saved buffer
    if (m_demux_pkt.pData)
    {
      pData = m_demux_pkt.pData;
      iSize = m_demux_pkt.iSize;
    }
  }

  if (pData)
  {
    // try to fetch an input buffer
    int64_t timeout_us = 5000;
    int index = m_codec->dequeueInputBuffer(timeout_us);
    if (xbmc_jnienv()->ExceptionCheck())
    {
      std::string err = CJNIBase::ExceptionToString();
      CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::Decode ExceptionCheck \n %s", err.c_str());
      rtn = 0;
    }
    else if (index >= 0)
    {
      // docs lie, getInputBuffers should be good after
      // m_codec->start() but the internal refs are not
      // setup until much later on some devices.
      if (m_input.empty())
      {
        m_input = m_codec->getInputBuffers();
        if (xbmc_jnienv()->ExceptionCheck())
        {
          CLog::Log(LOGERROR, "CDVDMediaCodecInfo::getInputBuffers "
            "ExceptionCheck");
          xbmc_jnienv()->ExceptionDescribe();
          xbmc_jnienv()->ExceptionClear();
        }
      }

      int size = m_input[index].capacity();
      if (iSize > size)
      {
        CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::Decode, iSize(%d) > size(%d)", iSize, size);
        iSize = size;
      }
      // fetch a pointer to the ByteBuffer backing store
      uint8_t *dst_ptr = (uint8_t*)xbmc_jnienv()->GetDirectBufferAddress(m_input[index].get_raw());
      if (dst_ptr)
      {
        // Codec specifics
        switch(m_hints.codec)
        {
          default:
            memcpy(dst_ptr, pData, iSize);
            break;
        }
        rtn = iSize;
      }


#ifdef DEBUG_VERBOSE
      CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec::Decode iSize(%d)", iSize);
#endif

      int flags = 0;
      int offset = 0;
      int64_t presentationTimeUs = 0;
      m_codec->queueInputBuffer(index, offset, iSize, presentationTimeUs, flags);
      // clear any jni exceptions, jni gets upset if we do not.
      if (xbmc_jnienv()->ExceptionCheck())
      {
        CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::Decode ExceptionCheck");
        xbmc_jnienv()->ExceptionClear();
      }

      // Free saved buffer it there was one
      if (m_demux_pkt.pData)
      {
        free(m_demux_pkt.pData);
        memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));
      }
    }
    else
    {
      // We couldn't get an input buffer. Save the packet for next iteration, if it wasn't already
      if (!m_demux_pkt.pData)
      {
        m_demux_pkt.iSize = iSize;
        m_demux_pkt.pData = (uint8_t*)malloc(iSize);
        memcpy(m_demux_pkt.pData, pData, iSize);
      }
    }
  }

  return rtn;
}

void CDVDAudioCodecAndroidMediaCodec::Reset()
{
  if (!m_opened)
    return;

  // dump any pending demux packets
  if (m_demux_pkt.pData)
  {
    free(m_demux_pkt.pData);
    memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));
  }

  if (m_codec)
  {
    // now we can flush the actual MediaCodec object
    m_codec->flush();
    if (xbmc_jnienv()->ExceptionCheck())
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::Reset ExceptionCheck");
      xbmc_jnienv()->ExceptionClear();
    }
  }
}

CAEChannelInfo CDVDAudioCodecAndroidMediaCodec::GetChannelMap()
{
  CAEChannelInfo chaninfo;

  for (int i=0; i<m_channels; ++i)
    chaninfo += KnownChannels[i];

  return chaninfo;
}

bool CDVDAudioCodecAndroidMediaCodec::ConfigureMediaCodec(void)
{
  // setup a MediaFormat to match the audio content,
  // used by codec during configure
  CJNIMediaFormat mediaformat = CJNIMediaFormat::createAudioFormat(
    m_mime.c_str(), m_hints.samplerate, m_hints.channels);

  // handle codec extradata
  if (m_hints.extrasize)
  {
    size_t size = m_hints.extrasize;
    void  *src_ptr = m_hints.extradata;
    // Allocate a byte buffer via allocateDirect in java instead of NewDirectByteBuffer,
    // since the latter doesn't allocate storage of its own, and we don't know how long
    // the codec uses the buffer.
    CJNIByteBuffer bytebuffer = CJNIByteBuffer::allocateDirect(size);
    void *dts_ptr = xbmc_jnienv()->GetDirectBufferAddress(bytebuffer.get_raw());
    memcpy(dts_ptr, src_ptr, size);
    // codec will automatically handle buffers as extradata
    // using entries with keys "csd-0", "csd-1", etc.
    mediaformat.setByteBuffer("csd-0", bytebuffer);
  }
  else if (m_hints.codec == AV_CODEC_ID_AAC || m_hints.codec == AV_CODEC_ID_AAC_LATM)
  {
    mediaformat.setInteger(CJNIMediaFormat::KEY_IS_ADTS, 1);
  }

  // configure and start the codec.
  // use the MediaFormat that we have setup.
  // use a null MediaCrypto, our content is not encrypted.
  // use a null Surface
  int flags = 0;
  CJNIMediaCrypto crypto(jni::jhobject(NULL));
  CJNISurface surface(jni::jhobject(NULL));
  m_codec->configure(mediaformat, surface, crypto, flags);
  // always, check/clear jni exceptions.
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::ExceptionCheck: configure");
    xbmc_jnienv()->ExceptionDescribe();
    xbmc_jnienv()->ExceptionClear();
    return false;
  }


  m_codec->start();
  // always, check/clear jni exceptions.
  if (xbmc_jnienv()->ExceptionCheck())
  {
    CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::ExceptionCheck: start");
    xbmc_jnienv()->ExceptionDescribe();
    xbmc_jnienv()->ExceptionClear();
    return false;
  }

  // There is no guarantee we'll get an INFO_OUTPUT_FORMAT_CHANGED (up to Android 4.3)
  // Configure the output with defaults
  ConfigureOutputFormat(&mediaformat);

  return true;
}

int CDVDAudioCodecAndroidMediaCodec::GetData(uint8_t** dst)
{
  m_bufferUsed = 0;

  int64_t timeout_us = 10000;
  CJNIMediaCodecBufferInfo bufferInfo;
  int index = m_codec->dequeueOutputBuffer(bufferInfo, timeout_us);
  if (xbmc_jnienv()->ExceptionCheck())
  {
    std::string err = CJNIBase::ExceptionToString();
    CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData ExceptionCheck; dequeueOutputBuffer \n %s", err.c_str());
    return 0;
  }
  if (index >= 0)
  {
    // some devices will return a valid index
    // before signaling INFO_OUTPUT_BUFFERS_CHANGED which
    // is used to setup m_output, D'uh. setup m_output here.
    if (m_output.empty())
    {
      m_output = m_codec->getOutputBuffers();
      if (xbmc_jnienv()->ExceptionCheck())
      {
        CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData ExceptionCheck: getOutputBuffers");
        xbmc_jnienv()->ExceptionDescribe();
        xbmc_jnienv()->ExceptionClear();
        return 0;
      }
    }

    int flags = bufferInfo.flags();
    if (flags & CJNIMediaCodec::BUFFER_FLAG_SYNC_FRAME)
      CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec:: BUFFER_FLAG_SYNC_FRAME");

    if (flags & CJNIMediaCodec::BUFFER_FLAG_CODEC_CONFIG)
      CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec:: BUFFER_FLAG_CODEC_CONFIG");

    if (flags & CJNIMediaCodec::BUFFER_FLAG_END_OF_STREAM)
    {
      CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec:: BUFFER_FLAG_END_OF_STREAM");
      m_codec->releaseOutputBuffer(index, false);
      if (xbmc_jnienv()->ExceptionCheck())
      {
        CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData ExceptionCheck: releaseOutputBuffer");
        xbmc_jnienv()->ExceptionDescribe();
        xbmc_jnienv()->ExceptionClear();
        return 0;
      }
      return 0;
    }

    int size = bufferInfo.size();
    int offset = bufferInfo.offset();

    if (!m_output[index].isDirect())
      CLog::Log(LOGWARNING, "CDVDAudioCodecAndroidMediaCodec:: m_output[index].isDirect == false");

    if (size && m_output[index].capacity())
    {
      uint8_t *src_ptr = (uint8_t*)xbmc_jnienv()->GetDirectBufferAddress(m_output[index].get_raw());
      src_ptr += offset;

      if (size > m_bufferSize)
      {
        m_bufferSize = size;
        m_buffer = (uint8_t*)realloc(m_buffer, m_bufferSize);
      }

      memcpy(m_buffer, src_ptr, size);
      m_bufferUsed = size;
    }
    else
      return 0;

    m_codec->releaseOutputBuffer(index, false);
    if (xbmc_jnienv()->ExceptionCheck())
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData ExceptionCheck: releaseOutputBuffer");
      xbmc_jnienv()->ExceptionDescribe();
      xbmc_jnienv()->ExceptionClear();
    }


#ifdef DEBUG_VERBOSE
    CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec::GetData "
      "index(%d), size(%d)", index, m_bufferUsed);
#endif

    // always, check/clear jni exceptions.
    if (xbmc_jnienv()->ExceptionCheck())
      xbmc_jnienv()->ExceptionClear();
  }
  else if (index == CJNIMediaCodec::INFO_OUTPUT_BUFFERS_CHANGED)
  {
    m_output = m_codec->getOutputBuffers();
    if (xbmc_jnienv()->ExceptionCheck())
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData(INFO_OUTPUT_BUFFERS_CHANGED) ExceptionCheck: getOutputBuffers");
      xbmc_jnienv()->ExceptionDescribe();
      xbmc_jnienv()->ExceptionClear();
    }
  }
  else if (index == CJNIMediaCodec::INFO_OUTPUT_FORMAT_CHANGED)
  {
    CJNIMediaFormat mediaformat = m_codec->getOutputFormat();
    if (xbmc_jnienv()->ExceptionCheck())
    {
      CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData(INFO_OUTPUT_FORMAT_CHANGED) ExceptionCheck: getOutputBuffers");
      xbmc_jnienv()->ExceptionDescribe();
      xbmc_jnienv()->ExceptionClear();
    }
    ConfigureOutputFormat(&mediaformat);
  }
  else if (index == CJNIMediaCodec::INFO_TRY_AGAIN_LATER)
  {
    // normal dequeueOutputBuffer timeout, ignore it.
    m_bufferUsed = 0;
  }
  else
  {
    // we should never get here
    CLog::Log(LOGERROR, "CDVDAudioCodecAndroidMediaCodec::GetData unknown index(%d)", index);
  }

  *dst     = m_buffer;
  return m_bufferUsed;
}

void CDVDAudioCodecAndroidMediaCodec::ConfigureOutputFormat(CJNIMediaFormat* mediaformat)
{
  m_samplerate       = 0;
  m_channels         = 0;

  if (mediaformat->containsKey("sample-rate"))
    m_samplerate       = mediaformat->getInteger("sample-rate");
  if (mediaformat->containsKey("channel-count"))
    m_channels     = mediaformat->getInteger("channel-count");

#if 1 //defined(DEBUG_VERBOSE)
  CLog::Log(LOGDEBUG, "CDVDAudioCodecAndroidMediaCodec:: "
    "sample_rate(%d), channel_count(%d)",
    m_samplerate, m_channels);
#endif

  // clear any jni exceptions
  if (xbmc_jnienv()->ExceptionCheck())
    xbmc_jnienv()->ExceptionClear();
}

