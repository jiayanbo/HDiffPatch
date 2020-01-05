//  sync_client.cpp
//  sync_client
//  Created by housisong on 2019-09-18.
/*
 The MIT License (MIT)
 Copyright (c) 2019-2020 HouSisong
 
 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation
 files (the "Software"), to deal in the Software without
 restriction, including without limitation the rights to use,
 copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following
 conditions:
 
 The above copyright notice and this permission notice shall be
 included in all copies of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
 */
#include "sync_client.h"
#include "../../file_for_patch.h"
#include "match_in_old.h"
#include "mt_by_queue.h"

#define check(v,errorCode) \
            do{ if (!(v)) { if (result==kSyncClient_ok) result=errorCode; \
                            if (!_inClear) goto clear; } }while(0)

struct _TWriteDatas {
    const hpatch_TStreamOutput* out_newStream;
    const TNewDataSyncInfo*     newSyncInfo;
    const hpatch_StreamPos_t*   newDataPoss;
    const hpatch_TStreamInput*  oldStream;
    hpatch_TDecompress*         decompressPlugin;
    hpatch_TChecksum*           strongChecksumPlugin;
    ISyncPatchListener*         listener;
};

static int mt_writeToNew(_TWriteDatas& wd,void* _mt=0,int threadIndex=0) {
    const TNewDataSyncInfo* newSyncInfo=wd.newSyncInfo;
    ISyncPatchListener*     listener=wd.listener;
    hpatch_TChecksum*       strongChecksumPlugin=wd.strongChecksumPlugin;
    int result=kSyncClient_ok;
    int _inClear=0;
    const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(newSyncInfo);
    const uint32_t kMatchBlockSize=newSyncInfo->kMatchBlockSize;
    const bool     isChecksumNewSyncData=listener->checksumSet.isChecksumNewSyncData;
    TByte*             dataBuf=0;
    TByte*             checksumSync_buf=0;
    hpatch_checksumHandle checksumSync=0;
    hpatch_StreamPos_t posInNewSyncData=0;
    hpatch_StreamPos_t outNewDataPos=0;
    const hpatch_StreamPos_t oldDataSize=wd.oldStream->streamSize;
    
    size_t _memSize=kMatchBlockSize*(wd.decompressPlugin?2:1)
                    +(isChecksumNewSyncData ? newSyncInfo->kStrongChecksumByteSize:0);
    dataBuf=(TByte*)malloc(_memSize);
    check(dataBuf!=0,kSyncClient_memError);
    if (isChecksumNewSyncData){
        checksumSync_buf=dataBuf+_memSize-newSyncInfo->kStrongChecksumByteSize;
        checksumSync=strongChecksumPlugin->open(strongChecksumPlugin);
        check(checksumSync!=0,kSyncClient_strongChecksumOpenError);
    }
    for (uint32_t syncSize,newDataSize,i=0; i<kBlockCount; ++i,
                            outNewDataPos+=newDataSize,posInNewSyncData+=syncSize) {
        syncSize=TNewDataSyncInfo_syncBlockSize(newSyncInfo,i);
        newDataSize=TNewDataSyncInfo_newDataBlockSize(newSyncInfo,i);
#if (_IS_USED_MULTITHREAD)
        if (_mt) { if (!((TMt_by_queue*)_mt)->getWork(threadIndex,i)) continue; } //next work;
#endif
        const hpatch_StreamPos_t curSyncPos=wd.newDataPoss[i];
        if (curSyncPos>=oldDataSize){ //needSync
            TByte* buf=(syncSize<newDataSize)?(dataBuf+kMatchBlockSize):dataBuf;
            if ((wd.out_newStream)||(listener)){
                {//read data
                    TSyncDataType cacheIndex=(curSyncPos==kBlockType_needSync)?kSyncDataType_needSync
                                                              :(curSyncPos-oldDataSize);
#if (_IS_USED_MULTITHREAD)
                    TMt_by_queue::TAutoInputLocker _autoLocker((TMt_by_queue*)_mt);
#endif
                    check(listener->readSyncData(listener,posInNewSyncData,syncSize,cacheIndex,buf),
                          kSyncClient_readSyncDataError);
                }
                if (syncSize<newDataSize){
                    check(hpatch_deccompress_mem(wd.decompressPlugin,buf,buf+syncSize,
                                                 dataBuf,dataBuf+newDataSize),kSyncClient_decompressError);
                }
                if (isChecksumNewSyncData){ //checksum
                    if (newDataSize<kMatchBlockSize)//for backZeroLen
                        memset(dataBuf+newDataSize,0,kMatchBlockSize-newDataSize);
                    strongChecksumPlugin->begin(checksumSync);
                    strongChecksumPlugin->append(checksumSync,dataBuf,dataBuf+kMatchBlockSize);
                    strongChecksumPlugin->end(checksumSync,checksumSync_buf,
                                              checksumSync_buf+newSyncInfo->kStrongChecksumByteSize);
                    toPartChecksum(checksumSync_buf,checksumSync_buf,newSyncInfo->kStrongChecksumByteSize);
                    check(0==memcmp(checksumSync_buf,
                                    newSyncInfo->partChecksums+i*(size_t)kPartStrongChecksumByteSize,
                                    kPartStrongChecksumByteSize),kSyncClient_checksumSyncDataError);
                }
            }
        }else{//copy from old
#if (_IS_USED_MULTITHREAD)
            TMt_by_queue::TAutoInputLocker _autoLocker((TMt_by_queue*)_mt); //can use other locker
#endif
            check(wd.oldStream->read(wd.oldStream,curSyncPos,dataBuf,dataBuf+newDataSize),
                  kSyncClient_readOldDataError);
        }
        if (wd.out_newStream){//write
#if (_IS_USED_MULTITHREAD)
            TMt_by_queue::TAutoOutputLocker _autoLocker((TMt_by_queue*)_mt,threadIndex,i);
#endif
            check(wd.out_newStream->write(wd.out_newStream,outNewDataPos,dataBuf,
                                          dataBuf+newDataSize), kSyncClient_writeNewDataError);
        }
    }
    assert(outNewDataPos==newSyncInfo->newDataSize);
    assert(posInNewSyncData==newSyncInfo->newSyncDataSize);
clear:
    _inClear=1;
    if (checksumSync) strongChecksumPlugin->close(strongChecksumPlugin,checksumSync);
    if (dataBuf) free(dataBuf);
    return result;
}


#if (_IS_USED_MULTITHREAD)
struct TMt_threadDatas{
    _TWriteDatas*       writeDatas;
    TMt_by_queue*       shareDatas;
    int                 result;
};

static void _mt_threadRunCallBackProc(int threadIndex,void* workData){
    TMt_threadDatas* tdatas=(TMt_threadDatas*)workData;
    int result=mt_writeToNew(*tdatas->writeDatas,tdatas->shareDatas,threadIndex);
    {//set result
        TMt_by_queue::TAutoLocker _auto_locker(tdatas->shareDatas);
        if (tdatas->result==kSyncClient_ok) tdatas->result=result;
    }
    tdatas->shareDatas->finish();
    bool isMainThread=(threadIndex==tdatas->shareDatas->threadNum-1);
    if (isMainThread) tdatas->shareDatas->waitAllFinish();
}
#endif

static int writeToNew(_TWriteDatas& writeDatas,int threadNum) {

#if (_IS_USED_MULTITHREAD)
    if (threadNum>1){
        const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(writeDatas.newSyncInfo);
        TMt_by_queue   shareDatas((int)threadNum,kBlockCount,true);
        TMt_threadDatas  tdatas;
        tdatas.shareDatas=&shareDatas;
        tdatas.writeDatas=&writeDatas;
        tdatas.result=kSyncClient_ok;
        thread_parallel((int)threadNum,_mt_threadRunCallBackProc,&tdatas,1);
        return tdatas.result;
    }else
#endif
    {
        return mt_writeToNew(writeDatas);
    }
}


struct TMatchedSyncInfo:public TNeedSyncInfo{
    inline explicit TMatchedSyncInfo(bool _isUseCache)
    :isUseCache(_isUseCache) { needSyncCount=0; needCacheSyncCount=0;
        needSyncSize=0; needCacheSyncSize=0; }
    const bool isUseCache;
};

static void setSameOldPos(hpatch_StreamPos_t* out_newDataPoss,TMatchedSyncInfo& msi,
                          const TNewDataSyncInfo* newSyncInfo,hpatch_StreamPos_t oldDataSize){
    static const hpatch_StreamPos_t kBlockType_repeat =kBlockType_needSync-1;
    
    uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(newSyncInfo);
    msi.needSyncCount=kBlockCount;
    msi.needSyncSize=newSyncInfo->newSyncDataSize;
    uint32_t curPair=0;
    for (uint32_t syncSize,i=0; i<kBlockCount; ++i){
        syncSize=TNewDataSyncInfo_syncBlockSize(newSyncInfo,i);
        if (out_newDataPoss[i]!=kBlockType_needSync){
            --msi.needSyncCount;
            msi.needSyncSize-=syncSize;
        }
        if ((curPair<newSyncInfo->samePairCount)
            &&(i==newSyncInfo->samePairList[curPair].curIndex)){
            assert(out_newDataPoss[i]==kBlockType_needSync);
            {
                uint32_t sameIndex=newSyncInfo->samePairList[curPair].sameIndex;
                hpatch_StreamPos_t syncInfo=out_newDataPoss[sameIndex];
                if (syncInfo<oldDataSize){
                    out_newDataPoss[i]=syncInfo; //ok from old
                    --msi.needSyncCount;
                    msi.needSyncSize-=syncSize;
                }else{
                    if (msi.isUseCache){
                        out_newDataPoss[i]=kBlockType_repeat;
                        out_newDataPoss[sameIndex]=kBlockType_repeat;
                        --msi.needSyncCount;
                        msi.needSyncSize-=syncSize;
                    } //else download,not cache
                }
            }
            ++curPair;
        }
    }
    assert(curPair==newSyncInfo->samePairCount);
    
    if (!msi.isUseCache) return;
    
    hpatch_StreamPos_t cacheIndex=0;
    curPair=0;
    for (uint32_t i=0; i<kBlockCount; ++i){
        if (out_newDataPoss[i]==kBlockType_repeat){
            out_newDataPoss[i]=oldDataSize+cacheIndex;
        }
        if ((curPair<newSyncInfo->samePairCount)
            &&(i==newSyncInfo->samePairList[curPair].curIndex)){
            {
                uint32_t sameIndex=newSyncInfo->samePairList[curPair].sameIndex;
                out_newDataPoss[i]=out_newDataPoss[sameIndex];
            }
            ++curPair;
        }
        if (out_newDataPoss[i]==oldDataSize+cacheIndex){
            ++msi.needCacheSyncCount;
            uint32_t syncSize=TNewDataSyncInfo_syncBlockSize(newSyncInfo,i);
            msi.needCacheSyncSize+=syncSize;
            ++cacheIndex;
        }
    }
    assert(curPair==newSyncInfo->samePairCount);
}

static void sendSyncMsg(ISyncPatchListener* listener,const hpatch_StreamPos_t* newDataPoss,
                        const TNewDataSyncInfo* newSyncInfo,const TMatchedSyncInfo& msi,hpatch_StreamPos_t oldDataSize){
    if (listener==0) return;
    if (listener->needSyncMsg)
        listener->needSyncMsg(listener,&msi);
    if (listener->needSyncDataMsg==0) return;
    
    const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(newSyncInfo);
    hpatch_StreamPos_t posInNewSyncData=0;
    for (uint32_t syncSize,i=0; i<kBlockCount; ++i,posInNewSyncData+=syncSize){
        syncSize=TNewDataSyncInfo_syncBlockSize(newSyncInfo,i);
        hpatch_StreamPos_t curSyncInfo=newDataPoss[i];
        if (curSyncInfo<oldDataSize) continue;
        TSyncDataType syncType=(curSyncInfo==kBlockType_needSync)?kSyncDataType_needSync
                                                                 :(curSyncInfo-oldDataSize);
        listener->needSyncDataMsg(listener,posInNewSyncData,syncSize,syncType);
    }
    assert(posInNewSyncData==newSyncInfo->newSyncDataSize);
}

static void printMatchResult(const TNewDataSyncInfo* newSyncInfo,const TMatchedSyncInfo& msi) {
    const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(newSyncInfo);
    printf("syncCount: %d (/%d=%.3f) (cacheCount: %d)  syncSize: %" PRIu64 "\n",
           msi.needSyncCount,kBlockCount,(double)msi.needSyncCount/kBlockCount,
           msi.needCacheSyncCount,msi.needSyncSize);
    hpatch_StreamPos_t downloadSize=newSyncInfo->newSyncInfoSize+msi.needSyncSize;
    printf("downloadSize: %" PRIu64 "+%" PRIu64 "= %" PRIu64 " (/%" PRIu64 "=%.3f)",
           newSyncInfo->newSyncInfoSize,msi.needSyncSize,downloadSize,
           newSyncInfo->newSyncDataSize,(double)downloadSize/newSyncInfo->newSyncDataSize);
    printf(" (/%" PRIu64 "=%.3f)\n",
           newSyncInfo->newDataSize,(double)downloadSize/newSyncInfo->newDataSize);
}

int sync_patch(ISyncPatchListener* listener,const hpatch_TStreamOutput* out_newStream,
               const hpatch_TStreamInput*  oldStream,const TNewDataSyncInfo* newSyncInfo,int threadNum){
    assert(listener!=0);
    hpatch_TDecompress* decompressPlugin=0;
    hpatch_TChecksum*   strongChecksumPlugin=0;
    const uint32_t kBlockCount=(uint32_t)TNewDataSyncInfo_blockCount(newSyncInfo);
    TMatchedSyncInfo matchedSyncInfo(listener->isCanCacheRepeatSyncData);
    hpatch_StreamPos_t* newDataPoss=0;
    int result=kSyncClient_ok;
    int _inClear=0;
    
    //decompressPlugin
    if (newSyncInfo->compressType){
        if ((newSyncInfo->_decompressPlugin!=0)
            &&(newSyncInfo->_decompressPlugin->is_can_open(newSyncInfo->compressType))){
            decompressPlugin=newSyncInfo->_decompressPlugin;
        }else{
            decompressPlugin=listener->findDecompressPlugin(listener,newSyncInfo->compressType);
            check(decompressPlugin!=0,kSyncClient_noDecompressPluginError);
        }
    }
    //strongChecksumPlugin
    if ((newSyncInfo->_strongChecksumPlugin!=0)
        &&(newSyncInfo->kStrongChecksumByteSize==newSyncInfo->_strongChecksumPlugin->checksumByteSize())
        &&(0==strcmp(newSyncInfo->strongChecksumType,newSyncInfo->_strongChecksumPlugin->checksumType()))){
        strongChecksumPlugin=newSyncInfo->_strongChecksumPlugin;
    }else{
        strongChecksumPlugin=listener->findChecksumPlugin(listener,newSyncInfo->strongChecksumType);
        check(strongChecksumPlugin!=0,kSyncClient_noStrongChecksumPluginError);
        check(strongChecksumPlugin->checksumByteSize()==newSyncInfo->kStrongChecksumByteSize,
              kSyncClient_strongChecksumByteSizeError);
    }

    //match in oldData
    newDataPoss=(hpatch_StreamPos_t*)malloc(kBlockCount*(size_t)sizeof(hpatch_StreamPos_t));
    check(newDataPoss!=0,kSyncClient_memError);
    try{
        matchNewDataInOld(newDataPoss,newSyncInfo,oldStream,strongChecksumPlugin,threadNum);
    }catch(...){
        result=kSyncClient_matchNewDataInOldError;
    }
    check(result==kSyncClient_ok,result);
    setSameOldPos(newDataPoss,matchedSyncInfo,newSyncInfo,oldStream->streamSize);
    printMatchResult(newSyncInfo,matchedSyncInfo);
    
    //send msg: all need sync block
    sendSyncMsg(listener,newDataPoss,newSyncInfo,matchedSyncInfo,oldStream->streamSize);
    
    _TWriteDatas writeDatas;
    writeDatas.out_newStream=out_newStream;
    writeDatas.newSyncInfo=newSyncInfo;
    writeDatas.newDataPoss=newDataPoss;
    writeDatas.oldStream=oldStream;
    writeDatas.decompressPlugin=decompressPlugin;
    writeDatas.strongChecksumPlugin=strongChecksumPlugin;
    writeDatas.listener=listener;
    result=writeToNew(writeDatas,threadNum);
    check(result==kSyncClient_ok,result);
clear:
    _inClear=1;
    if (newDataPoss) free(newDataPoss);
    return result;
}

int sync_patch_file2file(ISyncPatchListener* listener,const char* outNewFile,
                         const char* oldFile,const char* newSyncInfoFile,int threadNum){
    int result=kSyncClient_ok;
    int _inClear=0;
    TNewDataSyncInfo         newSyncInfo;
    hpatch_TFileStreamInput  oldData;
    hpatch_TFileStreamOutput out_newData;
    const hpatch_TStreamInput* oldStream=0;
    bool isOldPathInputEmpty=(oldFile==0)||(strlen(oldFile)==0);
    
    TNewDataSyncInfo_init(&newSyncInfo);
    hpatch_TFileStreamInput_init(&oldData);
    hpatch_TFileStreamOutput_init(&out_newData);
    result=TNewDataSyncInfo_open_by_file(&newSyncInfo,newSyncInfoFile,listener);
    check(result==kSyncClient_ok,result);
    
    if (!isOldPathInputEmpty)
        check(hpatch_TFileStreamInput_open(&oldData,oldFile),kSyncClient_oldFileOpenError);
    oldStream=&oldData.base;
    check(hpatch_TFileStreamOutput_open(&out_newData,outNewFile,(hpatch_StreamPos_t)(-1)),
          kSyncClient_newFileCreateError);
    
    result=sync_patch(listener,&out_newData.base,oldStream,&newSyncInfo,threadNum);
clear:
    _inClear=1;
    check(hpatch_TFileStreamOutput_close(&out_newData),kSyncClient_newFileCloseError);
    check(hpatch_TFileStreamInput_close(&oldData),kSyncClient_oldFileCloseError);
    TNewDataSyncInfo_close(&newSyncInfo);
    return result;
}
