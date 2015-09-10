//
// OpenALAudioPlayer.cpp
// OpenALとffmpegを使ったオーディオプレーヤーです。
// ffmpeg-2.7.2にて開発。
// 
// Copyright (C) 2015 Shun ITO <movingentity@gmail.com>
//
//
// Linux
// g++ OpenALAudioPlayer.cpp -lavcodec -lavformat -lavutil -lswresample -lopenal -o OpenALAudioPlayer
//
// MacOSX (下記OpenALヘッダのincludeの変更必要)
// g++ OpenALAudioPlayer.cpp -lavcodec -lavformat -lavutil -lswresample -framework OpenAL -o OpenALAudioPlayer
//
//
// 参考にしたサイト
// How to read an audio file with ffmpeg in c++? - General Programming - GameDev.net
// http://www.gamedev.net/topic/624876-how-to-read-an-audio-file-with-ffmpeg-in-c/
//
// An ffmpeg and SDL Tutorial
// http://dranger.com/ffmpeg/tutorial03.html
//


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
};

//#ifdef MACOSX
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
//#else
//#include <AL/al.h>
//#include <AL/alc.h>
//#endif

#include <iostream>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using namespace std;


#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio
#define NUM_BUFFERS 3

static int playing;

typedef struct PacketQueue {
  AVPacketList *pFirstPkt;
  AVPacketList *pLastPkt;
  int numOfPackets;
} PacketQueue;



void initPacketQueue(PacketQueue *q)
{
  memset(q, 0, sizeof(PacketQueue));
}


int pushPacketToPacketQueue(PacketQueue *pPktQ, AVPacket *pPkt)
{

  AVPacketList *pPktList;

  if (av_dup_packet(pPkt) < 0) {
    return -1;
  }

  pPktList = (AVPacketList*)av_malloc(sizeof(AVPacketList));
  if (!pPktList) {
    return -1;
  }

  pPktList->pkt = *pPkt;
  pPktList->next = NULL;
  
  
  //
  if (!pPktQ->pLastPkt) {
    pPktQ->pFirstPkt = pPktList;
  } else {
    pPktQ->pLastPkt->next = pPktList;
  }

  pPktQ->pLastPkt = pPktList;

  //
  pPktQ->numOfPackets++;

  return 0;
}


static int popPacketFromPacketQueue(PacketQueue *pPQ, AVPacket *pPkt)
{
  AVPacketList *pPktList;
  int ret;
  
  //
  pPktList = pPQ->pFirstPkt;
  
  if (pPktList) {
    pPQ->pFirstPkt = pPktList->next;
    if (!pPQ->pFirstPkt) {
      pPQ->pLastPkt = NULL;
    }
    pPQ->numOfPackets--;

    *pPkt = pPktList->pkt;

    av_free(pPktList);

    return 0;
  }

  return -1;
}


int decode(uint8_t* buf, int bufSize, AVPacket* packet, AVCodecContext* codecContext,
	   SwrContext *swr, int dstRate, int dstNbChannels, enum AVSampleFormat* dstSampleFmt)
{

  unsigned int bufIndex = 0;
  unsigned int dataSize = 0;
  
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    cout << "Error allocating the frame" << endl;
    av_free_packet(packet);
    return 0;
  }


  while (packet->size > 0)
  { 
    // デコードする。
    int gotFrame = 0;
    int result = avcodec_decode_audio4(codecContext, frame, &gotFrame, packet);
    
    if (result >= 0 && gotFrame) 
    {
	
      packet->size -= result;
      packet->data += result;	
    
      // 変換後のデータサイズを計算する。
      int dstNbSamples = av_rescale_rnd(frame->nb_samples, dstRate, codecContext->sample_rate, AV_ROUND_UP);
      uint8_t** dstData = NULL;
      int dstLineSize;
      if (av_samples_alloc_array_and_samples(&dstData, &dstLineSize, dstNbChannels, dstNbSamples, *dstSampleFmt, 0) < 0) {
	cerr << "Could not allocate destination samples" << endl;
	dataSize = 0;
	break;
      }
        
      // デコードしたデータをSwrContextに指定したフォーマットに変換する。
      int ret = swr_convert(swr, dstData, dstNbSamples, (const uint8_t **)frame->extended_data, frame->nb_samples);
      //int ret = swr_convert(swr, dstData, *dstNbSamples, (const uint8_t **)frame->data, frame->nb_samples);
      if (ret < 0) {
	cerr << "Error while converting" << endl;
	dataSize = 0;
	break;
      }

      // 変換したデータのサイズを計算する。
      int dstBufSize = av_samples_get_buffer_size(&dstLineSize, dstNbChannels, ret, *dstSampleFmt, 1);
      if (dstBufSize < 0) {
	cerr << "Error av_samples_get_buffer_size()" << endl;
	dataSize = 0;
	break;
      }      
	
      if (dataSize + dstBufSize > bufSize) {
	cerr << "dataSize + dstBufSize > bufSize" << endl;
	dataSize = 0;
	break;
      }

      // 変換したデータをバッファに書き込む
      memcpy((uint8_t *)buf + bufIndex, dstData[0], dstBufSize);
      bufIndex += dstBufSize;
      dataSize += dstBufSize;

      if (dstData)
	av_freep(&dstData[0]);
      av_freep(&dstData);
      
    } else {
      
      packet->size = 0;
      packet->data = NULL;
      
    }
  }

  av_free_packet(packet);
  av_free(frame);
  
  return dataSize;
}


static void sigHandler(int sig)
{
  // 再生終了
  playing = 0;
}


int main(int argc, char **argv)
{

  if (argc < 2) {
    cout << "Usage: " << argv[0] << " path" << endl;
    return 0;
  }

  // 引数チェック
  int j;
  for (j=1; j<argc; j++) {
    if (strcmp(argv[j], "-h") == 0 || strcmp(argv[j], "--help") == 0) {
      
      cout << endl;
      cout << "Usage: " <<  argv[0] << " path" << endl;
      cout << endl;
      cout << "OpenALとffmpegを使ったオーディオプレーヤーです。" << endl;
      cout << "pathにオーディオファイルか、オーディオファイルを含んだディレクトリへのパスを指定して実行します。" << endl;
      cout << "ディレクトリを指定した場合は、ディレクトリに含まれるオーディオファイルを順番に再生します。" << endl;
      cout << "Control-Zで、現在のオーディオファイルの再生を停止して、次のオーディオファイルを再生します。" << endl;
      cout << "\t" << "" << endl;

      return 0;
    }
  }

  playing = 0;

  // 与えられたパスが、ファイルかディレクトリか調べる。
  struct stat statbuf;
  if (stat(argv[1], &statbuf) == -1)
    return -1;

  if (S_ISREG(statbuf.st_mode)) {

    // 与えられたパスがファイルの場合
    cout << "File:" << argv[1] << endl;

    // 再帰的の呼ばれる場合は、
    // SIGTSTPシグナルが無視されるように設定されているので、
    // デフォルトの状態に復元する。
    struct sigaction saDefault;
    saDefault.sa_handler = sigHandler;
    saDefault.sa_flags = 0;
    sigemptyset(&saDefault.sa_mask);
    sigaction(SIGTSTP, &saDefault, NULL); //SIGINT

  } else if (S_ISDIR(statbuf.st_mode)) {

    // 与えられたパスがディレクトリの場合
    cout << "Directory:" << argv[1] << endl;

    // SIGTSTPシグナルが子プロセスでとらえられるようにするため、
    // SIGTSTPシグナルを無視するように設定する。
    struct sigaction saIgnore;
    saIgnore.sa_handler = SIG_IGN;
    saIgnore.sa_flags = 0;
    sigemptyset(&saIgnore.sa_mask);
    sigaction(SIGTSTP, &saIgnore, NULL); //SIGINT

    // 与えられたディレクトリを調べる。
    DIR *dirp;
    struct dirent *dp;
    dirp = opendir(argv[1]);
    if (dirp == NULL) {
      cerr << "opendir failed on " << argv[1] << endl;
      return -1;
    }

    // 見つかったファイル毎に、子プロセスを作成し、
    // 自プログラムを再帰的に実行する。
    for (;;) {
      dp = readdir(dirp);
      if (dp == NULL)
	break;

      // .と..は飛ばす。
      if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
	continue;

      // 新しいプロセスを作成する。
      int status;
      pid_t childPid;
      switch (childPid = fork()) {

      case -1:
	continue;

      case 0: {
	// 子プロセス
	string fullPath = string(argv[1]) + "/" + string(dp->d_name);
	execl(argv[0], argv[0], fullPath.c_str(), (char*)NULL);
	_exit(127);
      }
      default:
	// 親プロセス
	if (waitpid(childPid, &status, 0) == -1) 
	  continue;
	else
	  continue;
      }
    }
	
    if (closedir(dirp) == -1) {
      cerr << "closedir failed." << endl;
      return -1;
    }

    return 0;
  }
  

  //-----------------------------------------------------------------//


  // FFmpegのログレベルをQUIETに設定する。（何も出力しない）
  av_log_set_level(AV_LOG_QUIET);

  // FFmpegを初期化
  av_register_all();
  
  AVFormatContext* formatContext = NULL;
  if (avformat_open_input(&formatContext, argv[1], NULL, NULL) != 0) {
    cerr << "Error opening the file" << endl;
    return 1;
  }
  
  if (avformat_find_stream_info(formatContext, NULL) < 0) {
    avformat_close_input(&formatContext);
    cerr << "Error finding the stream info" << endl;
    return 1;
  }

  //
  AVCodec* cdc = NULL;
  int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &cdc, 0);
  if (streamIndex < 0) {
    avformat_close_input(&formatContext);
    cerr << "Could not find any audio stream in the file" << endl;
    return 1;
  }

  AVStream* audioStream = formatContext->streams[streamIndex];
  AVCodecContext* codecContext = audioStream->codec;
  codecContext = audioStream->codec;
  codecContext->codec = cdc;

  if (avcodec_open2(codecContext, codecContext->codec, NULL) != 0) {
    avformat_close_input(&formatContext);
    cout << "Couldn't open the context with the decoder" << endl;
    return 1;
  }
  
  cout << "This stream has " << codecContext->channels
       << " channels and a sample rate of " 
       << codecContext->sample_rate << "Hz" << endl;
  cout << "The data is in the format " << av_get_sample_fmt_name(codecContext->sample_fmt) << endl;


  //-----------------------------------------------------------------//


  // Set up SWR context once you've got codec information
  int dstRate = codecContext->sample_rate;
  int dstNbChannels = 0;
  int64_t dstChLayout = AV_CH_LAYOUT_STEREO;
  enum AVSampleFormat dstSampleFmt = AV_SAMPLE_FMT_S16;


  // buffer is going to be directly written to a rawaudio file, no alignment
  dstNbChannels = av_get_channel_layout_nb_channels(dstChLayout);

  struct SwrContext *swr = swr_alloc();
  if (!swr) {
    fprintf(stderr, "Could not allocate resampler context\n");
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);
    return 1;
  }

  // チャンネルレイアウトがわからない場合は、チャンネル数から取得する。
  if (codecContext->channel_layout == 0)
    codecContext->channel_layout = av_get_default_channel_layout( codecContext->channels );

  //
  av_opt_set_int(swr, "in_channel_layout", codecContext->channel_layout, 0);
  av_opt_set_int(swr, "out_channel_layout", dstChLayout,  0);
  av_opt_set_int(swr, "in_sample_rate", codecContext->sample_rate, 0);
  av_opt_set_int(swr, "out_sample_rate", dstRate, 0);
  av_opt_set_sample_fmt(swr, "in_sample_fmt", codecContext->sample_fmt, 0);
  av_opt_set_sample_fmt(swr, "out_sample_fmt", dstSampleFmt,  0);
  if (swr_init(swr) < 0) {
    cerr << "Failed to initialize the resampling context" << endl;
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);
    return 1;
  }


  //-----------------------------------------------------------------//


  ALCdevice *dev;
  ALCcontext *ctx;

  dev = alcOpenDevice(NULL);
  if(!dev) {
    fprintf(stderr, "Oops\n");
    swr_free(&swr);
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);
    return 1;
  }
  
  ctx = alcCreateContext(dev, NULL);
  alcMakeContextCurrent(ctx);
  if(!ctx) {
    fprintf(stderr, "Oops2\n");
    swr_free(&swr);
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);
    return 1;
  }
  
  ALuint source, buffers[NUM_BUFFERS];

  alGenBuffers(NUM_BUFFERS, buffers);
  alGenSources(1, &source);
  if(alGetError() != AL_NO_ERROR) {
    fprintf(stderr, "Error generating :(\n");
    swr_free(&swr);
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);
    return 1;
  }
  

  // PacketQueueを使い
  // 一旦音楽データをパケットに分解して保持する。
  PacketQueue pktQueue;
  initPacketQueue(&pktQueue);

  AVPacket readingPacket;
  av_init_packet(&readingPacket);

  while (av_read_frame(formatContext, &readingPacket)>=0) {

    // Is this a packet from the video stream?
    if(readingPacket.stream_index == audioStream->index) {

      pushPacketToPacketQueue(&pktQueue, &readingPacket);

    } else {
      av_free_packet(&readingPacket);
    }
  }


  uint8_t audioBuf[AVCODEC_MAX_AUDIO_FRAME_SIZE];
  unsigned int audioBufSize = 0;

  // NUM_BUFFERSの数だけ、あらかじめバッファを準備する。
  int i;
  for (i=0; i<NUM_BUFFERS; i++) {

    // パケットキューにたまっているパケットを取得する。
    AVPacket decodingPacket;
    if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0) {
      cerr << "error." << endl;
      break;
    }

    // 取得したパケットをデコードして、希望のフォーマットに変換する。
    audioBufSize = decode(&audioBuf[0], sizeof(audioBuf), &decodingPacket,
			  codecContext, swr, dstRate, dstNbChannels, &dstSampleFmt);


    // デコード、変換したデータを、OpenALのバッファに書き込む。
    alBufferData(buffers[i], AL_FORMAT_STEREO16, audioBuf, audioBufSize, dstRate);
    if (alGetError() != AL_NO_ERROR) {
      cerr << "Error Buffer :(" << endl;
      av_free_packet(&decodingPacket);
      continue;
    }    

    av_free_packet(&decodingPacket);
  }
  

  // データが入ったバッファをキューに追加して、再生を開始する。
  alSourceQueueBuffers(source, NUM_BUFFERS, buffers);
  alSourcePlay(source);
  
  if (alGetError() != AL_NO_ERROR) {
    cerr << "Error starting." << endl;
    return 1;
  } else {
    cout << "Playing.." << endl;
    playing = 1;
  }
  

  // パケットキューがなくなるまで、繰り返す。
  while (pktQueue.numOfPackets && playing) {

    // 使用済みバッファの数を取得する。
    // 使用済みバッファがない場合は、できるまで繰り返す。
    ALint val;
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &val);
    if(val <= 0) {
      // 少しスリープさせて処理を減らす。
      struct timespec ts = {0, 1 * 1000000}; // 1msec
      nanosleep(&ts, NULL);
      continue;
    }

    AVPacket decodingPacket;
    if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0) {
      cerr << "error." << endl;
      break;
    }

    audioBufSize = decode(&audioBuf[0], sizeof(audioBuf), &decodingPacket,
			  codecContext, swr, dstRate, dstNbChannels, &dstSampleFmt);

    if (audioBufSize <= 0) {
      continue;
    }

    // 再生済みのバッファをデキューする。
    ALuint buffer;
    alSourceUnqueueBuffers(source, 1, &buffer);
    
    // デキューしたバッファに、新しい音楽データを書き込む。
    alBufferData(buffer, AL_FORMAT_STEREO16, audioBuf, audioBufSize, dstRate);
    if(alGetError() != AL_NO_ERROR)
    {
      fprintf(stderr, "Error Buffer :(\n");
      return 1;
    }	    
    
    // 新しいデータを書き込んだバッファをキューする。
    alSourceQueueBuffers(source, 1, &buffer);
    if (alGetError() != AL_NO_ERROR)
    {
      fprintf(stderr, "Error buffering :(\n");
      return 1;
    }

    // もし再生が止まっていたら、再生する。
    alGetSourcei(source, AL_SOURCE_STATE, &val);
    if (val != AL_PLAYING)
      alSourcePlay(source);
    
    // 掃除
    av_free_packet(&decodingPacket);
  }


  // 未処理のパケットが残っている場合は、パケットを解放する。
  while (pktQueue.numOfPackets) {
    AVPacket decodingPacket;
    if (popPacketFromPacketQueue(&pktQueue, &decodingPacket) < 0)
      continue;
    av_free_packet(&decodingPacket);
  }


  cout << "End." << endl;


  swr_free(&swr);

  avformat_close_input(&formatContext);
  avcodec_close(codecContext);
  avformat_free_context(formatContext);

  alDeleteSources(1, &source);
  alDeleteBuffers(NUM_BUFFERS, buffers);

  alcMakeContextCurrent(NULL);
  alcDestroyContext(ctx);
  alcCloseDevice(dev);

  return 0;
}
