// Copyright (C) 2019. Huawei Technologies Co., Ltd. All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), 
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE 
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR 
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


/**
 * Project deploy
 */
#ifndef _LSTMCELL_H
#define _LSTMCELL_H

#include "weight_operator.hpp"
#include "tensor_computing.h"


class LSTMCell: public WeightOperator {
public:
    LSTMCell(DataType dt, U32 numOutput, U32 numProjection, F32 zoneoutCell, F32 zoneoutOutput, bool biDirection)
    {
        this->dt = dt;
        this->lstmDesc.numOutput = numOutput;
        this->lstmDesc.numProjection = numProjection;
        this->lstmDesc.forgetBias = 1.0;
        this->lstmDesc.zoneoutCell = zoneoutCell;
        this->lstmDesc.zoneoutOutput = zoneoutOutput;
        this->lstmDesc.biDirection = biDirection;
        this->lstmDesc.activationMode = ACTIVATION_TANH;
        this->hasBias = false;
    }

    OperatorType get_op_type() override
    {
        return OT_LSTM;
    }

    void run() override
    {
        UTIL_TIME_TIC(__CLASS_FUNCTION__)
        Tensor xTensor      = this->inputTensors[0];
        U8* xPtr = (U8*)xTensor.get_val();
        Tensor weightTensor = this->weightTensors[0];
        Tensor biasTensor   = this->biasTensors[0];
        Tensor stateTensor  = this->inputTensors[1];
        Tensor hTensor      = this->outputTensors[0];

        U8 *tempPtr = (U8*)this->temp->get_val();

        if (this->featureScale.size() > 1) {
            CHECK_STATUS(clip(&(this->clipMin), &(this->clipMax),
                          xTensor.get_desc(), xTensor.get_val(),
                          xTensor.get_desc(), tempPtr, this->schedule));
            xPtr = tempPtr;
            tempPtr += tensorNumBytes(xTensor.get_desc());
        }

        CHECK_STATUS(lstmcell(xTensor.get_desc(), xPtr,
                              weightTensor.get_desc(), weightTensor.get_val(),
                              biasTensor.get_desc(), biasTensor.get_val(),
                              stateTensor.get_val(),
                              this->lenOfTemp, tempPtr,
                              this->lstmDesc, this->xDim, this->lstmDesc.numOutput,
                              hTensor.get_desc(), hTensor.get_val(), this->schedule));

        UTIL_TIME_TOC(__CLASS_FUNCTION__)
    }

    EE infer_output_tensors_size(Vec<TensorDesc> inDims, Vec<TensorDesc>* outDims) override
    {
        TensorDesc inDim = inDims[0];

        DataType dt;
        DataFormat df;
        U32 iB, iX;
        CHECK_STATUS(tensor2dfGet(inDim, &dt, &df, &iB, &iX));
        U32 column = (this->lstmDesc.numProjection > 0) ? this->lstmDesc.numProjection : this->lstmDesc.numOutput;
        this->xDim = iX;
        this->filterRow = 4 * column;
        this->filterCol = this->lstmDesc.numOutput + iX;
        TensorDesc filter_dim = tensor2df(this->dt, DF_NK, this->filterRow, this->filterCol);
      	U32 outBytes = 0;
        CHECK_STATUS(lstmcell_infer_output_size(inDim, filter_dim, this->lstmDesc, &((*outDims)[0]), &outBytes));
        return SUCCESS;
    }

    U32 infer_tmp_memory_size() override
    {
        TensorDesc inputDesc = (this->inputTensors[0]).get_desc();
        TensorDesc filterDesc = (this->weightTensors[0]).get_desc();
        TensorDesc outputDesc = (this->outputTensors[0]).get_desc();
        U32 bytes = 0;
        CHECK_STATUS(lstmcell_infer_forward_tmp_bytes(inputDesc, filterDesc, outputDesc, this->lstmDesc, &bytes, this->schedule));

        if (featureScale.size() > 1) {
            CHECK_REQUIREMENT(featureScale[0][0] > 0);
            CHECK_REQUIREMENT(featureScale[0][0] == featureScale[1][0]);
            this->clipMax = 127.0 / featureScale[0][0];
            this->clipMin = -1 * this->clipMax;

            bytes += tensorNumBytes(inputDesc);
        }
        return bytes;
    }

    U32 infer_wtm_memory_size() override
    {
        TensorDesc filterDesc = (this->weightTensors[0]).get_desc();
        U32 byte = 0;
        CHECK_STATUS(lstm_transform_filter_bytes(filterDesc, this->lstmDesc, &byte, this->schedule));
        return byte;
    }
	
    EE transform_filter()
    {
        this->wtm = std::shared_ptr<Tensor>(new Tensor());
        Tensor weightTensor = this->weightTensors[0];
        TensorDesc weightDesc = weightTensor.get_desc();
        U8* weightPtr = weightTensor.get_val();
    
        auto wtmBytes = this->infer_wtm_memory_size();
        std::shared_ptr<U8> wtmPtr((U8*) operator new(wtmBytes));
        auto cpuMem = new CpuMemory();
        cpuMem->set_shared_ptr_caster(wtmPtr);
        Memory_* mem = (Memory_*)(cpuMem);
        std::shared_ptr<Memory_> memWtmPtr(mem);
        this->set_wtm_memory(wtmBytes, memWtmPtr);
    
        TensorDesc wtmDesc;
        CHECK_STATUS(lstm_transform_filter(weightDesc, weightPtr, this->lstmDesc, &wtmDesc, this->get_wtm()->get_val(), this->schedule));

        this->get_wtm()->set_desc(wtmDesc);
        this->weightTensors[0] = *this->get_wtm();
        return SUCCESS;
    }

    EE init_weight_bias_from_model(U8** modelPtr)
    {
        int num = (this->lstmDesc.biDirection) ? 2 : 1;
        U32 row = this->xDim + this->lstmDesc.numOutput;
        U32 column = (this->lstmDesc.numProjection > 0) ? this->lstmDesc.numProjection : this->lstmDesc.numOutput;
        TensorDesc weightDesc = tensor2df(this->dt, DF_NK, this->filterRow, this->filterCol);
        TensorDesc biasDesc = tensor1d(this->dt, column * 4);
        U32 weightBytes = num * bytesOf(this->dt) * ((row * column * 4) + (this->lstmDesc.numProjection * this->lstmDesc.numOutput));
        U32 biasBytes = num * tensorNumBytes(biasDesc);

        std::shared_ptr<Tensor> modelWeightTensor(new Tensor());
        std::shared_ptr<Tensor> modelBiasTensor(new Tensor());
        modelWeightTensor->set_desc(weightDesc);
        modelBiasTensor->set_desc(biasDesc);

        auto curOpWs = this->get_weightspec_ptr();
        if(modelPtr != nullptr){
            modelWeightTensor->alloc();
            memcpy((U8*)modelWeightTensor->get_val(), *modelPtr, weightBytes);
            *modelPtr += weightBytes;
        } else {
            modelWeightTensor->set_shared_ptr(std::shared_ptr<U8>(curOpWs.weight));
        }

        U8* biasVal = nullptr;
        if(modelPtr != nullptr){
            if(this->hasBias){
                biasVal = *modelPtr;
                *modelPtr += biasBytes;
            }
        } else {
            if(this->hasBias) biasVal = curOpWs.vec; 
        }

        if (biasVal) {
            modelBiasTensor->set_shared_ptr(std::shared_ptr<U8>(biasVal));
        } else {
            modelBiasTensor->alloc();
            memset((U8*)modelBiasTensor->get_val(), 0, biasBytes);
        }

        this->weightTensors.push_back(*modelWeightTensor.get());
        this->biasTensors.push_back(*modelBiasTensor.get());
        return SUCCESS;
    }

public:
    LSTMDesc lstmDesc;
    U32 filterRow;
    U32 filterCol;
    U32 xDim;
    F32 clipMax;
    F32 clipMin;
};

#endif //_LSTMCELL_H
