#include "accompany_decoder_controller.h"

#define LOG_TAG "AccompanyDecoderController"
AccompanyDecoderController::AccompanyDecoderController() {
	accompanyDecoder = NULL;
	playPosition = 0.0f;
    currentAccompanyPacket = NULL;
    currentAccompanyPacketCursor = 0;
}

AccompanyDecoderController::~AccompanyDecoderController() {
    if(currentAccompanyPacket) {
        delete currentAccompanyPacket;
        currentAccompanyPacket = NULL;
    }
}

int AccompanyDecoderController::getMusicMeta(const char* accompanyPath, int * accompanyMetaData) {
	// 创建临时的解码器
    AccompanyDecoder* accompanyDecoder = new AccompanyDecoder();
    //获取伴奏的meta，包含采样率和比特率
    accompanyDecoder->getMusicMeta(accompanyPath, accompanyMetaData);
    // 销毁临时的解码器
	delete accompanyDecoder;
	// 初始化伴奏的采样率
	accompanySampleRate = accompanyMetaData[0];
//	LOGI("accompanySampleRate is %d", accompanySampleRate);
	return 0;
}

float AccompanyDecoderController::getPlayPosition(){
	return playPosition;
}

float AccompanyDecoderController::seekToPosition(float position){
	LOGI("enter AccompanyDecoderController::seekToPosition() position=%f", position);
	float actualSeekPosition = -1;
	if (NULL != accompanyDecoder) {
		//暂停解码线程
		pthread_mutex_lock(&mDecodePausingLock);
		pthread_mutex_lock(&mLock);
		isDecodePausingFlag = true;
		pthread_cond_signal(&mCondition);
		pthread_mutex_unlock(&mLock);

		pthread_cond_wait(&mDecodePausingCondition, &mDecodePausingLock);
		packetPool->clearDecoderAccompanyPacketToQueue();
		accompanyDecoder->setPosition(position);
		isDecodePausingFlag = false;
		pthread_mutex_unlock(&mDecodePausingLock);

		pthread_mutex_lock(&mLock);
		pthread_cond_signal(&mCondition);
		pthread_mutex_unlock(&mLock);

		while((actualSeekPosition = accompanyDecoder->getActualSeekPosition()) == -1){
			usleep(10 * 1000);
		}

		LOGI("exit AccompanyDecoderController::seekToPosition() position=%f, actualSeekPosition=%f", position, actualSeekPosition);
	}
	return actualSeekPosition;
}

void* AccompanyDecoderController::startDecoderThread(void* ptr) {
	LOGI("enter AccompanyDecoderController::startDecoderThread");
	AccompanyDecoderController* decoderController = (AccompanyDecoderController *) ptr;
	//上普通的互斥锁
	int getLockCode = pthread_mutex_lock(&decoderController->mLock);
	// 当还在执行任务的时候，持续循环
	while (decoderController->isRunning) {
	    // 如果标记了解码暂停
		if (decoderController->isDecodePausingFlag) {
		    // 上暂停相关的互斥锁
			pthread_mutex_lock(&decoderController->mDecodePausingLock);
			// 释放暂停相关的条件锁
			pthread_cond_signal(&decoderController->mDecodePausingCondition);
			// 释放暂停相关的互斥锁
			pthread_mutex_unlock(&decoderController->mDecodePausingLock);
            // 等待普通的条件锁满足后释放普通锁
			pthread_cond_wait(&decoderController->mCondition, &decoderController->mLock);
		}
		else {
		    // 解码
			decoderController->decodeSongPacket();
			// 如果解码后的包队列大于阈值
			if (decoderController->packetPool->geDecoderAccompanyPacketQueueSize() > QUEUE_SIZE_MAX_THRESHOLD) {
			    // 等待普通的条件锁满足后释放普通锁
				pthread_cond_wait(&decoderController->mCondition, &decoderController->mLock);
			}
		}
	}
	// 释放普通锁
	pthread_mutex_unlock(&decoderController->mLock);
    return 0;
}

void AccompanyDecoderController::initAccompanyDecoder(const char* accompanyPath) {
	//	LOGI("accompanyByteCountPerSec is %d packetBufferTimePercent is %f accompanyPacketBufferSize is %d", accompanyByteCountPerSec, packetBufferTimePercent, accompanyPacketBufferSize);
	//初始化两个decoder
    // 构建解码器
	accompanyDecoder = new AccompanyDecoder();
    // 初始化解码器
	accompanyDecoder->init(accompanyPath, accompanyPacketBufferSize);
}

void AccompanyDecoderController::init(const char* accompanyPath, float packetBufferTimePercent) {
	//初始化两个全局变量
	volume = 1.0f;
	accompanyMax = 1.0f;

    int meta[2];
    // 获取采样率和比特率
    this->getMusicMeta(accompanyPath, meta);
	// 根据采样率计算出伴奏和原唱的bufferSize
	int accompanyByteCountPerSec = accompanySampleRate * CHANNEL_PER_FRAME * BITS_PER_CHANNEL / BITS_PER_BYTE;
	accompanyPacketBufferSize = (int) ((accompanyByteCountPerSec / 2) * packetBufferTimePercent);

//	LOGI("accompanyByteCountPerSec is %d packetBufferTimePercent is %f accompanyPacketBufferSize is %d", accompanyByteCountPerSec, packetBufferTimePercent, accompanyPacketBufferSize);
	//初始化解码器
	initAccompanyDecoder(accompanyPath);
	// 获取队列实例
	packetPool = PacketPool::GetInstance();
    // 初始化队列
	packetPool->initDecoderAccompanyPacketQueue();
    // 初始化解码线程
	initDecoderThread();
}

void AccompanyDecoderController::initDecoderThread() {
    // 设置正在运行
	isRunning = true;
    // 设置解码没有被暂停的标记
	isDecodePausingFlag = false;
    // 初始化互斥锁
	pthread_mutex_init(&mLock, NULL);
    // 初始化条件锁
	pthread_cond_init(&mCondition, NULL);
    // 初始化解码暂停的互斥锁
	pthread_mutex_init(&mDecodePausingLock, NULL);
    // 初始化解码暂停的条件锁
	pthread_cond_init(&mDecodePausingCondition, NULL);
    // 创建线程，并设置线程开始时执行的函数
	pthread_create(&songDecoderThread, NULL, startDecoderThread, this);
}

//需要子类完成自己的操作
void AccompanyDecoderController::decodeSongPacket() {
//	LOGI("start AccompanyDecoderController::decodeSongPacket");
//	long readLatestFrameTimemills = getCurrentTime();
    // 解码
	AudioPacket* accompanyPacket = accompanyDecoder->decodePacket();
	// 设置包状态为播放
	accompanyPacket->action = AudioPacket::AUDIO_PACKET_ACTION_PLAY;
	// 将包放到队列中
	packetPool->pushDecoderAccompanyPacketToQueue(accompanyPacket);
//	LOGI("decode accompany Packet waste time mills is %d", (getCurrentTime() - readLatestFrameTimemills));
}

void AccompanyDecoderController::destroyDecoderThread() {
//	LOGI("enter AccompanyDecoderController::destoryProduceThread ....");
    // 设置当前没有运行
	isRunning = false;
    // 设置解码没有被暂停的标记
	isDecodePausingFlag = false;
	void* status;
    // 上互斥锁
	int getLockCode = pthread_mutex_lock(&mLock);
    // 告知条件锁满足条件，可释放
	pthread_cond_signal (&mCondition);
    // 释放互斥锁
	pthread_mutex_unlock (&mLock);
    // 等待解码线程结束并获取状态
	pthread_join(songDecoderThread, &status);
    // 销毁互斥锁
	pthread_mutex_destroy(&mLock);
    // 销毁条件锁
	pthread_cond_destroy(&mCondition);

    // 上解码暂停的互斥锁
	pthread_mutex_lock(&mDecodePausingLock);
    // 告知解码暂停的条件锁满足条件，可释放
	pthread_cond_signal(&mDecodePausingCondition);
    // 释放解码暂停的互斥锁
	pthread_mutex_unlock(&mDecodePausingLock);
    // 销毁解码暂停的互斥锁
	pthread_mutex_destroy(&mDecodePausingLock);
    // 销毁解码暂停的条件锁
	pthread_cond_destroy(&mDecodePausingCondition);
}

int AccompanyDecoderController::readSamples(short* samples, int size) {
	int result = -1;
    int fillCuror = 0;
    int sampleCursor = 0;
    // 持续工作，直到产出了指定大小
    while(fillCuror < size) {
        int samplePacketSize = 0;
        // 如果已经完成当前包的解码则清理指针
        if(currentAccompanyPacket && currentAccompanyPacketCursor == currentAccompanyPacket->size) {
            delete currentAccompanyPacket;
            currentAccompanyPacket = NULL;
        }
        // 如果还未完成当前包的解码
        if(currentAccompanyPacket && currentAccompanyPacketCursor < currentAccompanyPacket->size) {
            // 算出剩余长度
            int subSize = size - fillCuror;
            // 算出合适的可存放空间大小
            samplePacketSize = MIN(currentAccompanyPacket->size - currentAccompanyPacketCursor, subSize);
            
            memcpy(samples + fillCuror, currentAccompanyPacket->buffer + currentAccompanyPacketCursor, samplePacketSize * 2);
        } else {
            packetPool->getDecoderAccompanyPacket(&currentAccompanyPacket, true);
            currentAccompanyPacketCursor = 0;
            if (NULL != currentAccompanyPacket && currentAccompanyPacket->size > 0) {
                samplePacketSize = size - fillCuror;
                memcpy(samples + fillCuror, currentAccompanyPacket->buffer + currentAccompanyPacketCursor, samplePacketSize * 2);
            } else {
                result = -2;
                break;
            }
        }
        currentAccompanyPacketCursor += samplePacketSize;
        fillCuror += samplePacketSize;
    }
	
	if(packetPool->geDecoderAccompanyPacketQueueSize() < QUEUE_SIZE_MIN_THRESHOLD){
		pthread_mutex_lock(&mLock);
//        if (result != -1) {
			pthread_cond_signal(&mCondition);
//        }
		pthread_mutex_unlock (&mLock);
	}
	return result;
}

void AccompanyDecoderController::destroy() {
    // 销毁解码线程
	destroyDecoderThread();
    // 终止解码队列
	packetPool->abortDecoderAccompanyPacketQueue();
    // 销毁解码队列
	packetPool->destoryDecoderAccompanyPacketQueue();
    // 销毁解码器
	if (NULL != accompanyDecoder) {
		accompanyDecoder->destroy();
		delete accompanyDecoder;
		accompanyDecoder = NULL;
	}
}
