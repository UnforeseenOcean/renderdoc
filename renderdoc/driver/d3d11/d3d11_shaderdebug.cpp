/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2018 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "data/resource.h"
#include "driver/d3d11/d3d11_renderstate.h"
#include "driver/d3d11/d3d11_resources.h"
#include "driver/shaders/dxbc/dxbc_debug.h"
#include "maths/formatpacking.h"
#include "maths/vec.h"
#include "strings/string_utils.h"
#include "d3d11_context.h"
#include "d3d11_debug.h"
#include "d3d11_manager.h"
#include "d3d11_shader_cache.h"

// over this number of cycles and things get problematic
#define SHADER_DEBUG_WARN_THRESHOLD 100000

bool PromptDebugTimeout(DXBC::ProgramType prog, uint32_t cycleCounter)
{
  string msg = StringFormat::Fmt(
      "RenderDoc's shader debugging has been running for over %u cycles, which indicates either a "
      "very long-running loop, or possibly an infinite loop. Continuing could lead to extreme "
      "memory allocations, slow UI or even crashes. Would you like to abort debugging to see what "
      "has run so far?\n\n"
      "Hit yes to abort debugging. Note that loading the resulting trace could take several "
      "minutes.",
      cycleCounter);

  int ret = MessageBoxA(NULL, msg.c_str(), "Shader debugging timeout", MB_YESNO | MB_ICONWARNING);

  if(ret == IDYES)
    return true;

  return false;
}

ShaderDebug::State D3D11DebugManager::CreateShaderDebugState(ShaderDebugTrace &trace, int quadIdx,
                                                             DXBC::DXBCFile *dxbc, bytebuf *cbufData)
{
  using namespace DXBC;
  using namespace ShaderDebug;

  State initialState = State(quadIdx, &trace, dxbc, m_pDevice);

  // use pixel shader here to get inputs

  int32_t maxReg = -1;
  for(size_t i = 0; i < dxbc->m_InputSig.size(); i++)
    maxReg = RDCMAX(maxReg, (int32_t)dxbc->m_InputSig[i].regIndex);

  bool inputCoverage = false;

  for(size_t i = 0; i < dxbc->GetNumDeclarations(); i++)
  {
    const DXBC::ASMDecl &decl = dxbc->GetDeclaration(i);

    if(decl.declaration == OPCODE_DCL_INPUT && decl.operand.type == TYPE_INPUT_COVERAGE_MASK)
    {
      inputCoverage = true;
      break;
    }
  }

  if(maxReg >= 0 || inputCoverage)
  {
    trace.inputs.resize(maxReg + 1 + (inputCoverage ? 1 : 0));
    for(size_t i = 0; i < dxbc->m_InputSig.size(); i++)
    {
      SigParameter &sig = dxbc->m_InputSig[i];

      ShaderVariable v;

      v.name = StringFormat::Fmt("v%d (%s)", sig.regIndex, sig.semanticIdxName.c_str());
      v.rows = 1;
      v.columns = sig.regChannelMask & 0x8 ? 4 : sig.regChannelMask & 0x4
                                                     ? 3
                                                     : sig.regChannelMask & 0x2
                                                           ? 2
                                                           : sig.regChannelMask & 0x1 ? 1 : 0;

      if(sig.compType == CompType::UInt)
        v.type = VarType::UInt;
      else if(sig.compType == CompType::SInt)
        v.type = VarType::Int;

      if(trace.inputs[sig.regIndex].columns == 0)
        trace.inputs[sig.regIndex] = v;
      else
        trace.inputs[sig.regIndex].columns = RDCMAX(trace.inputs[sig.regIndex].columns, v.columns);
    }

    if(inputCoverage)
    {
      trace.inputs[maxReg + 1] = ShaderVariable("vCoverage", 0U, 0U, 0U, 0U);
      trace.inputs[maxReg + 1].columns = 1;
    }
  }

  uint32_t specialOutputs = 0;
  maxReg = -1;
  for(size_t i = 0; i < dxbc->m_OutputSig.size(); i++)
  {
    if(dxbc->m_OutputSig[i].regIndex == ~0U)
      specialOutputs++;
    else
      maxReg = RDCMAX(maxReg, (int32_t)dxbc->m_OutputSig[i].regIndex);
  }

  if(maxReg >= 0 || specialOutputs > 0)
  {
    initialState.outputs.resize(maxReg + 1 + specialOutputs);
    for(size_t i = 0; i < dxbc->m_OutputSig.size(); i++)
    {
      SigParameter &sig = dxbc->m_OutputSig[i];

      if(sig.regIndex == ~0U)
        continue;

      ShaderVariable v;

      v.name = StringFormat::Fmt("o%d (%s)", sig.regIndex, sig.semanticIdxName.c_str());
      v.rows = 1;
      v.columns = sig.regChannelMask & 0x8 ? 4 : sig.regChannelMask & 0x4
                                                     ? 3
                                                     : sig.regChannelMask & 0x2
                                                           ? 2
                                                           : sig.regChannelMask & 0x1 ? 1 : 0;

      if(initialState.outputs[sig.regIndex].columns == 0)
        initialState.outputs[sig.regIndex] = v;
      else
        initialState.outputs[sig.regIndex].columns =
            RDCMAX(initialState.outputs[sig.regIndex].columns, v.columns);
    }

    int32_t outIdx = maxReg + 1;

    for(size_t i = 0; i < dxbc->m_OutputSig.size(); i++)
    {
      SigParameter &sig = dxbc->m_OutputSig[i];

      if(sig.regIndex != ~0U)
        continue;

      ShaderVariable v;

      if(sig.systemValue == ShaderBuiltin::OutputControlPointIndex)
        v.name = "vOutputControlPointID";
      else if(sig.systemValue == ShaderBuiltin::DepthOutput)
        v.name = "oDepth";
      else if(sig.systemValue == ShaderBuiltin::DepthOutputLessEqual)
        v.name = "oDepthLessEqual";
      else if(sig.systemValue == ShaderBuiltin::DepthOutputGreaterEqual)
        v.name = "oDepthGreaterEqual";
      else if(sig.systemValue == ShaderBuiltin::MSAACoverage)
        v.name = "oMask";
      // if(sig.systemValue == TYPE_OUTPUT_CONTROL_POINT)							str = "oOutputControlPoint";
      else
      {
        RDCERR("Unhandled output: %s (%d)", sig.semanticName.c_str(), sig.systemValue);
        continue;
      }

      v.rows = 1;
      v.columns = sig.regChannelMask & 0x8 ? 4 : sig.regChannelMask & 0x4
                                                     ? 3
                                                     : sig.regChannelMask & 0x2
                                                           ? 2
                                                           : sig.regChannelMask & 0x1 ? 1 : 0;

      initialState.outputs[outIdx++] = v;
    }
  }

  trace.constantBlocks.resize(dxbc->m_CBuffers.size());
  for(size_t i = 0; i < dxbc->m_CBuffers.size(); i++)
  {
    if(dxbc->m_CBuffers[i].descriptor.type != CBuffer::Descriptor::TYPE_CBUFFER)
      continue;

    std::vector<ShaderVariable> vars;

    size_t dummy = 0;
    FillCBufferVariables("", dummy, dxbc->m_CBuffers[i].variables, vars, true,
                         cbufData[dxbc->m_CBuffers[i].reg]);

    trace.constantBlocks[i].members = vars;

    for(size_t c = 0; c < trace.constantBlocks[i].members.size(); c++)
      trace.constantBlocks[i].members[c].name =
          StringFormat::Fmt("cb%u[%u] (%s)", dxbc->m_CBuffers[i].reg, (uint32_t)c,
                            trace.constantBlocks[i].members[c].name.c_str());
  }

  initialState.Init();

  return initialState;
}

void D3D11DebugManager::CreateShaderGlobalState(ShaderDebug::GlobalState &global,
                                                DXBC::DXBCFile *dxbc, uint32_t UAVStartSlot,
                                                ID3D11UnorderedAccessView **UAVs,
                                                ID3D11ShaderResourceView **SRVs)
{
  for(int i = 0; UAVs != NULL && i + UAVStartSlot < D3D11_1_UAV_SLOT_COUNT; i++)
  {
    int dsti = i + UAVStartSlot;
    if(UAVs[i])
    {
      ID3D11Resource *res = NULL;
      UAVs[i]->GetResource(&res);

      global.uavs[dsti].hiddenCounter = GetStructCount(UAVs[i]);

      D3D11_UNORDERED_ACCESS_VIEW_DESC udesc;
      UAVs[i]->GetDesc(&udesc);

      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

      if(udesc.Format != DXGI_FORMAT_UNKNOWN)
        format = udesc.Format;

      if(format == DXGI_FORMAT_UNKNOWN)
      {
        if(WrappedID3D11Texture1D::IsAlloc(res))
        {
          D3D11_TEXTURE1D_DESC desc;
          ((WrappedID3D11Texture1D *)res)->GetDesc(&desc);
          format = desc.Format;
        }
        else if(WrappedID3D11Texture2D1::IsAlloc(res))
        {
          D3D11_TEXTURE2D_DESC desc;
          ((WrappedID3D11Texture2D1 *)res)->GetDesc(&desc);
          format = desc.Format;
        }
        else if(WrappedID3D11Texture3D1::IsAlloc(res))
        {
          D3D11_TEXTURE3D_DESC desc;
          ((WrappedID3D11Texture3D1 *)res)->GetDesc(&desc);
          format = desc.Format;
        }
      }

      if(format != DXGI_FORMAT_UNKNOWN)
      {
        ResourceFormat fmt = MakeResourceFormat(GetTypedFormat(udesc.Format));

        global.uavs[dsti].format.byteWidth = fmt.compByteWidth;
        global.uavs[dsti].format.numComps = fmt.compCount;
        global.uavs[dsti].format.fmt = fmt.compType;

        if(udesc.Format == DXGI_FORMAT_R11G11B10_FLOAT)
          global.uavs[dsti].format.byteWidth = 11;
        if(udesc.Format == DXGI_FORMAT_R10G10B10A2_UINT ||
           udesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
          global.uavs[dsti].format.byteWidth = 10;
      }

      if(udesc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
      {
        global.uavs[dsti].firstElement = udesc.Buffer.FirstElement;
        global.uavs[dsti].numElements = udesc.Buffer.NumElements;
      }

      if(res)
      {
        if(WrappedID3D11Buffer::IsAlloc(res))
        {
          GetBufferData((ID3D11Buffer *)res, 0, 0, global.uavs[dsti].data);
        }
        else
        {
          global.uavs[dsti].tex = true;

          uint32_t &rowPitch = global.uavs[dsti].rowPitch;
          uint32_t &depthPitch = global.uavs[dsti].depthPitch;

          bytebuf &data = global.uavs[dsti].data;

          if(udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE1D ||
             udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY)
          {
            D3D11_TEXTURE1D_DESC desc;
            ((WrappedID3D11Texture1D *)res)->GetDesc(&desc);

            desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;

            ID3D11Texture1D *stagingTex = NULL;
            m_pDevice->CreateTexture1D(&desc, NULL, &stagingTex);

            m_pImmediateContext->CopyResource(stagingTex, res);

            D3D11_MAPPED_SUBRESOURCE mapped;
            m_pImmediateContext->Map(stagingTex, udesc.Texture1D.MipSlice, D3D11_MAP_READ, 0,
                                     &mapped);

            rowPitch = 0;
            depthPitch = 0;
            size_t datasize = GetByteSize(desc.Width, 1, 1, desc.Format, udesc.Texture1D.MipSlice);

            uint32_t numSlices = 1;

            byte *srcdata = (byte *)mapped.pData;
            if(udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE1DARRAY)
            {
              rowPitch = mapped.RowPitch;
              srcdata += udesc.Texture1DArray.FirstArraySlice * rowPitch;
              numSlices = udesc.Texture1DArray.ArraySize;
              datasize = numSlices * rowPitch;
            }

            data.resize(datasize);

            // copy with all padding etc intact
            memcpy(&data[0], srcdata, datasize);

            m_pImmediateContext->Unmap(stagingTex, udesc.Texture1D.MipSlice);

            SAFE_RELEASE(stagingTex);
          }
          else if(udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D ||
                  udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
          {
            D3D11_TEXTURE2D_DESC desc;
            ((WrappedID3D11Texture2D1 *)res)->GetDesc(&desc);

            desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;

            ID3D11Texture2D *stagingTex = NULL;
            m_pDevice->CreateTexture2D(&desc, NULL, &stagingTex);

            m_pImmediateContext->CopyResource(stagingTex, res);

            // MipSlice in union is shared between Texture2D and Texture2DArray unions, so safe to
            // use either
            D3D11_MAPPED_SUBRESOURCE mapped;
            m_pImmediateContext->Map(stagingTex, udesc.Texture2D.MipSlice, D3D11_MAP_READ, 0,
                                     &mapped);

            rowPitch = mapped.RowPitch;
            depthPitch = 0;
            size_t datasize = rowPitch * RDCMAX(1U, desc.Height >> udesc.Texture2D.MipSlice);

            uint32_t numSlices = 1;

            byte *srcdata = (byte *)mapped.pData;
            if(udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
            {
              depthPitch = mapped.DepthPitch;
              srcdata += udesc.Texture2DArray.FirstArraySlice * depthPitch;
              numSlices = udesc.Texture2DArray.ArraySize;
              datasize = numSlices * depthPitch;
            }

            // copy with all padding etc intact
            data.resize(datasize);

            memcpy(&data[0], srcdata, datasize);

            m_pImmediateContext->Unmap(stagingTex, udesc.Texture2D.MipSlice);

            SAFE_RELEASE(stagingTex);
          }
          else if(udesc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE3D)
          {
            D3D11_TEXTURE3D_DESC desc;
            ((WrappedID3D11Texture3D1 *)res)->GetDesc(&desc);

            desc.MiscFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;

            ID3D11Texture3D *stagingTex = NULL;
            m_pDevice->CreateTexture3D(&desc, NULL, &stagingTex);

            m_pImmediateContext->CopyResource(stagingTex, res);

            // MipSlice in union is shared between Texture2D and Texture2DArray unions, so safe to
            // use either
            D3D11_MAPPED_SUBRESOURCE mapped;
            m_pImmediateContext->Map(stagingTex, udesc.Texture3D.MipSlice, D3D11_MAP_READ, 0,
                                     &mapped);

            rowPitch = mapped.RowPitch;
            depthPitch = mapped.DepthPitch;

            byte *srcdata = (byte *)mapped.pData;
            srcdata += udesc.Texture3D.FirstWSlice * mapped.DepthPitch;
            uint32_t numSlices = udesc.Texture3D.WSize;
            size_t datasize = depthPitch * numSlices;

            data.resize(datasize);

            // copy with all padding etc intact
            memcpy(&data[0], srcdata, datasize);

            m_pImmediateContext->Unmap(stagingTex, udesc.Texture3D.MipSlice);

            SAFE_RELEASE(stagingTex);
          }
        }
      }

      SAFE_RELEASE(res);
    }
  }

  for(int i = 0; SRVs != NULL && i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; i++)
  {
    if(SRVs[i])
    {
      ID3D11Resource *res = NULL;
      SRVs[i]->GetResource(&res);

      D3D11_SHADER_RESOURCE_VIEW_DESC sdesc;
      SRVs[i]->GetDesc(&sdesc);

      if(sdesc.Format != DXGI_FORMAT_UNKNOWN)
      {
        ResourceFormat fmt = MakeResourceFormat(sdesc.Format);

        global.srvs[i].format.byteWidth = fmt.compByteWidth;
        global.srvs[i].format.numComps = fmt.compCount;
        global.srvs[i].format.fmt = fmt.compType;

        if(sdesc.Format == DXGI_FORMAT_R11G11B10_FLOAT)
          global.srvs[i].format.byteWidth = 11;
        if(sdesc.Format == DXGI_FORMAT_R10G10B10A2_UINT ||
           sdesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
          global.srvs[i].format.byteWidth = 10;
      }
      else
      {
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);

        if(dim == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
          ID3D11Buffer *buf = (ID3D11Buffer *)res;
          D3D11_BUFFER_DESC bufdesc;
          buf->GetDesc(&bufdesc);

          global.srvs[i].format.stride = bufdesc.StructureByteStride;

          // if we didn't get a type from the SRV description, try to pull it from the declaration
          for(const DXBC::ShaderInputBind &bind : dxbc->m_SRVs)
          {
            if(bind.reg == (uint32_t)i && bind.dimension == DXBC::ShaderInputBind::DIM_BUFFER &&
               bind.retType < DXBC::RETURN_TYPE_MIXED && bind.retType != DXBC::RETURN_TYPE_UNKNOWN)
            {
              global.srvs[i].format.byteWidth = 4;
              global.srvs[i].format.numComps = bind.numSamples;

              if(bind.retType == DXBC::RETURN_TYPE_UNORM)
                global.srvs[i].format.fmt = CompType::UNorm;
              else if(bind.retType == DXBC::RETURN_TYPE_SNORM)
                global.srvs[i].format.fmt = CompType::SNorm;
              else if(bind.retType == DXBC::RETURN_TYPE_UINT)
                global.srvs[i].format.fmt = CompType::UInt;
              else if(bind.retType == DXBC::RETURN_TYPE_SINT)
                global.srvs[i].format.fmt = CompType::SInt;
              else
                global.srvs[i].format.fmt = CompType::Float;

              break;
            }
          }
        }
      }

      if(sdesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
      {
        // I know this isn't what the docs say, but as best as I can tell
        // this is how it's used.
        global.srvs[i].firstElement = sdesc.Buffer.FirstElement;
        global.srvs[i].numElements = sdesc.Buffer.NumElements;
      }
      else if(sdesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
      {
        global.srvs[i].firstElement = sdesc.BufferEx.FirstElement;
        global.srvs[i].numElements = sdesc.BufferEx.NumElements;
      }

      if(res)
      {
        if(WrappedID3D11Buffer::IsAlloc(res))
        {
          GetBufferData((ID3D11Buffer *)res, 0, 0, global.srvs[i].data);
        }
      }

      SAFE_RELEASE(res);
    }
  }

  for(size_t i = 0; i < dxbc->GetNumDeclarations(); i++)
  {
    const DXBC::ASMDecl &decl = dxbc->GetDeclaration(i);

    if(decl.declaration == DXBC::OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW ||
       decl.declaration == DXBC::OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED)
    {
      uint32_t slot = (uint32_t)decl.operand.indices[0].index;

      if(global.groupshared.size() <= slot)
      {
        global.groupshared.resize(slot + 1);

        ShaderDebug::GlobalState::groupsharedMem &mem = global.groupshared[slot];

        mem.structured = (decl.declaration == DXBC::OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED);

        mem.count = decl.count;
        if(mem.structured)
          mem.bytestride = decl.stride;
        else
          mem.bytestride = 4;    // raw groupshared is implicitly uint32s

        mem.data.resize(mem.bytestride * mem.count);
      }
    }
  }
}

// struct that saves pointers as we iterate through to where we ultimately
// want to copy the data to
struct DataOutput
{
  DataOutput(int regster, int element, int numWords, ShaderBuiltin attr, bool inc)
  {
    reg = regster;
    elem = element;
    numwords = numWords;
    sysattribute = attr;
    included = inc;
  }

  int reg;
  int elem;
  ShaderBuiltin sysattribute;

  int numwords;

  bool included;
};

struct DebugHit
{
  uint32_t numHits;
  float posx;
  float posy;
  float depth;
  uint32_t primitive;
  uint32_t isFrontFace;
  uint32_t sample;
  uint32_t coverage;
  uint32_t rawdata;    // arbitrary, depending on shader
};

ShaderDebugTrace D3D11Replay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                          uint32_t idx, uint32_t instOffset, uint32_t vertOffset)
{
  using namespace DXBC;
  using namespace ShaderDebug;

  D3D11MarkerRegion debugpixRegion(
      StringFormat::Fmt("DebugVertex @ %u of (%u,%u,%u)", eventId, vertid, instid, idx));

  ShaderDebugTrace empty;

  const DrawcallDescription *draw = m_pDevice->GetDrawcall(eventId);

  D3D11RenderStateTracker tracker(m_pImmediateContext);

  ID3D11VertexShader *stateVS = NULL;
  m_pImmediateContext->VSGetShader(&stateVS, NULL, NULL);

  WrappedID3D11Shader<ID3D11VertexShader> *vs = (WrappedID3D11Shader<ID3D11VertexShader> *)stateVS;

  SAFE_RELEASE(stateVS);

  if(!vs)
    return empty;

  DXBCFile *dxbc = vs->GetDXBC();

  if(!dxbc)
    return empty;

  dxbc->GetDisassembly();

  D3D11RenderState *rs = m_pImmediateContext->GetCurrentPipelineState();

  vector<D3D11_INPUT_ELEMENT_DESC> inputlayout = m_pDevice->GetLayoutDesc(rs->IA.Layout);

  set<UINT> vertexbuffers;
  uint32_t trackingOffs[32] = {0};

  UINT MaxStepRate = 1U;

  // need special handling for other step rates
  for(size_t i = 0; i < inputlayout.size(); i++)
  {
    if(inputlayout[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA &&
       inputlayout[i].InstanceDataStepRate < draw->numInstances)
      MaxStepRate = RDCMAX(inputlayout[i].InstanceDataStepRate, MaxStepRate);

    UINT slot =
        RDCCLAMP(inputlayout[i].InputSlot, 0U, UINT(D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1));

    vertexbuffers.insert(slot);

    if(inputlayout[i].AlignedByteOffset == ~0U)
    {
      inputlayout[i].AlignedByteOffset = trackingOffs[slot];
    }
    else
    {
      trackingOffs[slot] = inputlayout[i].AlignedByteOffset;
    }

    ResourceFormat fmt = MakeResourceFormat(inputlayout[i].Format);

    trackingOffs[slot] += fmt.compByteWidth * fmt.compCount;
  }

  bytebuf vertData[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  bytebuf *instData = new bytebuf[MaxStepRate * D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  bytebuf staticData[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];

  for(auto it = vertexbuffers.begin(); it != vertexbuffers.end(); ++it)
  {
    UINT i = *it;
    if(rs->IA.VBs[i])
    {
      GetDebugManager()->GetBufferData(rs->IA.VBs[i],
                                       rs->IA.Offsets[i] + rs->IA.Strides[i] * (vertOffset + idx),
                                       rs->IA.Strides[i], vertData[i]);

      for(UINT isr = 1; isr <= MaxStepRate; isr++)
      {
        GetDebugManager()->GetBufferData(
            rs->IA.VBs[i], rs->IA.Offsets[i] + rs->IA.Strides[i] * (instOffset + (instid / isr)),
            rs->IA.Strides[i], instData[i * MaxStepRate + isr - 1]);
      }

      GetDebugManager()->GetBufferData(rs->IA.VBs[i],
                                       rs->IA.Offsets[i] + rs->IA.Strides[i] * instOffset,
                                       rs->IA.Strides[i], staticData[i]);
    }
  }

  bytebuf cbufData[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];

  for(int i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
    if(rs->VS.ConstantBuffers[i])
      GetDebugManager()->GetBufferData(rs->VS.ConstantBuffers[i],
                                       rs->VS.CBOffsets[i] * sizeof(Vec4f), 0, cbufData[i]);

  ShaderDebugTrace ret;

  GlobalState global;
  GetDebugManager()->CreateShaderGlobalState(global, dxbc, 0, NULL, rs->VS.SRVs);
  State initialState = GetDebugManager()->CreateShaderDebugState(ret, -1, dxbc, cbufData);

  for(size_t i = 0; i < ret.inputs.size(); i++)
  {
    if(dxbc->m_InputSig[i].systemValue == ShaderBuiltin::Undefined ||
       dxbc->m_InputSig[i].systemValue ==
           ShaderBuiltin::Position)    // SV_Position seems to get promoted
                                       // automatically, but it's invalid for
                                       // vertex input
    {
      const D3D11_INPUT_ELEMENT_DESC *el = NULL;

      std::string signame = strlower(dxbc->m_InputSig[i].semanticName);

      for(size_t l = 0; l < inputlayout.size(); l++)
      {
        std::string layoutname = strlower(inputlayout[l].SemanticName);

        if(signame == layoutname && dxbc->m_InputSig[i].semanticIndex == inputlayout[l].SemanticIndex)
        {
          el = &inputlayout[l];
          break;
        }
        if(signame == layoutname + ToStr(inputlayout[l].SemanticIndex))
        {
          el = &inputlayout[l];
          break;
        }
      }

      RDCASSERT(el);

      if(!el)
        continue;

      byte *srcData = NULL;
      size_t dataSize = 0;

      if(el->InputSlotClass == D3D11_INPUT_PER_VERTEX_DATA)
      {
        if(vertData[el->InputSlot].size() >= el->AlignedByteOffset)
        {
          srcData = &vertData[el->InputSlot][el->AlignedByteOffset];
          dataSize = vertData[el->InputSlot].size() - el->AlignedByteOffset;
        }
      }
      else
      {
        if(el->InstanceDataStepRate == 0 || el->InstanceDataStepRate >= draw->numInstances)
        {
          srcData = &staticData[el->InputSlot][el->AlignedByteOffset];
          dataSize = staticData[el->InputSlot].size() - el->AlignedByteOffset;
        }
        else
        {
          UINT isrIdx = el->InputSlot * MaxStepRate + (el->InstanceDataStepRate - 1);
          if(instData[isrIdx].size() >= el->AlignedByteOffset)
          {
            srcData = &instData[isrIdx][el->AlignedByteOffset];
            dataSize = instData[isrIdx].size() - el->AlignedByteOffset;
          }
        }
      }

      ResourceFormat fmt = MakeResourceFormat(el->Format);

      // more data needed than is provided
      if(dxbc->m_InputSig[i].compCount > fmt.compCount)
      {
        ret.inputs[i].value.u.w = 1;

        if(fmt.compType == CompType::Float)
          ret.inputs[i].value.f.w = 1.0f;
      }

      // interpret resource format types
      if(fmt.Special())
      {
        Vec3f *v3 = (Vec3f *)ret.inputs[i].value.fv;
        Vec4f *v4 = (Vec4f *)ret.inputs[i].value.fv;

        // only pull in all or nothing from these,
        // if there's only e.g. 3 bytes remaining don't read and unpack some of
        // a 4-byte resource format type
        size_t packedsize = 4;
        if(fmt.type == ResourceFormatType::R5G5B5A1 || fmt.type == ResourceFormatType::R5G6B5 ||
           fmt.type == ResourceFormatType::R4G4B4A4)
          packedsize = 2;

        if(srcData == NULL || packedsize > dataSize)
        {
          ret.inputs[i].value.u.x = ret.inputs[i].value.u.y = ret.inputs[i].value.u.z =
              ret.inputs[i].value.u.w = 0;
        }
        else if(fmt.type == ResourceFormatType::R5G5B5A1)
        {
          RDCASSERT(fmt.bgraOrder);
          uint16_t packed = ((uint16_t *)srcData)[0];
          *v4 = ConvertFromB5G5R5A1(packed);
        }
        else if(fmt.type == ResourceFormatType::R5G6B5)
        {
          RDCASSERT(fmt.bgraOrder);
          uint16_t packed = ((uint16_t *)srcData)[0];
          *v3 = ConvertFromB5G6R5(packed);
        }
        else if(fmt.type == ResourceFormatType::R4G4B4A4)
        {
          RDCASSERT(fmt.bgraOrder);
          uint16_t packed = ((uint16_t *)srcData)[0];
          *v4 = ConvertFromB4G4R4A4(packed);
        }
        else if(fmt.type == ResourceFormatType::R10G10B10A2)
        {
          uint32_t packed = ((uint32_t *)srcData)[0];

          if(fmt.compType == CompType::UInt)
          {
            ret.inputs[i].value.u.z = (packed >> 0) & 0x3ff;
            ret.inputs[i].value.u.y = (packed >> 10) & 0x3ff;
            ret.inputs[i].value.u.x = (packed >> 20) & 0x3ff;
            ret.inputs[i].value.u.w = (packed >> 30) & 0x003;
          }
          else
          {
            *v4 = ConvertFromR10G10B10A2(packed);
          }
        }
        else if(fmt.type == ResourceFormatType::R11G11B10)
        {
          uint32_t packed = ((uint32_t *)srcData)[0];
          *v3 = ConvertFromR11G11B10(packed);
        }
      }
      else
      {
        for(uint32_t c = 0; c < fmt.compCount; c++)
        {
          if(srcData == NULL || fmt.compByteWidth > dataSize)
          {
            ret.inputs[i].value.uv[c] = 0;
            continue;
          }

          dataSize -= fmt.compByteWidth;

          if(fmt.compByteWidth == 1)
          {
            byte *src = srcData + c * fmt.compByteWidth;

            if(fmt.compType == CompType::UInt)
              ret.inputs[i].value.uv[c] = *src;
            else if(fmt.compType == CompType::SInt)
              ret.inputs[i].value.iv[c] = *((int8_t *)src);
            else if(fmt.compType == CompType::UNorm)
              ret.inputs[i].value.fv[c] = float(*src) / 255.0f;
            else if(fmt.compType == CompType::SNorm)
            {
              signed char *schar = (signed char *)src;

              // -128 is mapped to -1, then -127 to -127 are mapped to -1 to 1
              if(*schar == -128)
                ret.inputs[i].value.fv[c] = -1.0f;
              else
                ret.inputs[i].value.fv[c] = float(*schar) / 127.0f;
            }
            else
              RDCERR("Unexpected component type");
          }
          else if(fmt.compByteWidth == 2)
          {
            uint16_t *src = (uint16_t *)(srcData + c * fmt.compByteWidth);

            if(fmt.compType == CompType::Float)
              ret.inputs[i].value.fv[c] = ConvertFromHalf(*src);
            else if(fmt.compType == CompType::UInt)
              ret.inputs[i].value.uv[c] = *src;
            else if(fmt.compType == CompType::SInt)
              ret.inputs[i].value.iv[c] = *((int16_t *)src);
            else if(fmt.compType == CompType::UNorm)
              ret.inputs[i].value.fv[c] = float(*src) / float(UINT16_MAX);
            else if(fmt.compType == CompType::SNorm)
            {
              int16_t *sint = (int16_t *)src;

              // -32768 is mapped to -1, then -32767 to -32767 are mapped to -1 to 1
              if(*sint == -32768)
                ret.inputs[i].value.fv[c] = -1.0f;
              else
                ret.inputs[i].value.fv[c] = float(*sint) / 32767.0f;
            }
            else
              RDCERR("Unexpected component type");
          }
          else if(fmt.compByteWidth == 4)
          {
            uint32_t *src = (uint32_t *)(srcData + c * fmt.compByteWidth);

            if(fmt.compType == CompType::Float || fmt.compType == CompType::UInt ||
               fmt.compType == CompType::SInt)
              memcpy(&ret.inputs[i].value.uv[c], src, 4);
            else
              RDCERR("Unexpected component type");
          }
        }

        if(fmt.bgraOrder)
        {
          RDCASSERT(fmt.compCount == 4);
          std::swap(ret.inputs[i].value.fv[2], ret.inputs[i].value.fv[0]);
        }
      }
    }
    else if(dxbc->m_InputSig[i].systemValue == ShaderBuiltin::VertexIndex)
    {
      uint32_t sv_vertid = vertid;

      if(draw->flags & DrawFlags::UseIBuffer)
        sv_vertid = idx;

      if(dxbc->m_InputSig[i].compType == CompType::Float)
        ret.inputs[i].value.f.x = ret.inputs[i].value.f.y = ret.inputs[i].value.f.z =
            ret.inputs[i].value.f.w = (float)sv_vertid;
      else
        ret.inputs[i].value.u.x = ret.inputs[i].value.u.y = ret.inputs[i].value.u.z =
            ret.inputs[i].value.u.w = sv_vertid;
    }
    else if(dxbc->m_InputSig[i].systemValue == ShaderBuiltin::InstanceIndex)
    {
      if(dxbc->m_InputSig[i].compType == CompType::Float)
        ret.inputs[i].value.f.x = ret.inputs[i].value.f.y = ret.inputs[i].value.f.z =
            ret.inputs[i].value.f.w = (float)instid;
      else
        ret.inputs[i].value.u.x = ret.inputs[i].value.u.y = ret.inputs[i].value.u.z =
            ret.inputs[i].value.u.w = instid;
    }
    else
    {
      RDCERR("Unhandled system value semantic on VS input");
    }
  }

  delete[] instData;

  State last;

  vector<ShaderDebugState> states;

  states.push_back((State)initialState);

  D3D11MarkerRegion simloop("Simulation Loop");

  for(int cycleCounter = 0;; cycleCounter++)
  {
    if(initialState.Finished())
      break;

    initialState = initialState.GetNext(global, NULL);

    states.push_back((State)initialState);

    if(cycleCounter == SHADER_DEBUG_WARN_THRESHOLD)
    {
      if(PromptDebugTimeout(DXBC::TYPE_VERTEX, cycleCounter))
        break;
    }
  }

  ret.states = states;

  return ret;
}

ShaderDebugTrace D3D11Replay::DebugPixel(uint32_t eventId, uint32_t x, uint32_t y, uint32_t sample,
                                         uint32_t primitive)
{
  using namespace DXBC;
  using namespace ShaderDebug;

  D3D11MarkerRegion debugpixRegion(
      StringFormat::Fmt("DebugPixel @ %u of (%u,%u) %u / %u", eventId, x, y, sample, primitive));

  ShaderDebugTrace empty;

  D3D11RenderStateTracker tracker(m_pImmediateContext);

  ID3D11PixelShader *statePS = NULL;
  m_pImmediateContext->PSGetShader(&statePS, NULL, NULL);

  WrappedID3D11Shader<ID3D11PixelShader> *ps = (WrappedID3D11Shader<ID3D11PixelShader> *)statePS;

  SAFE_RELEASE(statePS);

  ID3D11GeometryShader *stateGS = NULL;
  m_pImmediateContext->GSGetShader(&stateGS, NULL, NULL);

  WrappedID3D11Shader<ID3D11GeometryShader> *gs =
      (WrappedID3D11Shader<ID3D11GeometryShader> *)stateGS;

  SAFE_RELEASE(stateGS);

  ID3D11DomainShader *stateDS = NULL;
  m_pImmediateContext->DSGetShader(&stateDS, NULL, NULL);

  WrappedID3D11Shader<ID3D11DomainShader> *ds = (WrappedID3D11Shader<ID3D11DomainShader> *)stateDS;

  SAFE_RELEASE(stateDS);

  ID3D11VertexShader *stateVS = NULL;
  m_pImmediateContext->VSGetShader(&stateVS, NULL, NULL);

  WrappedID3D11Shader<ID3D11VertexShader> *vs = (WrappedID3D11Shader<ID3D11VertexShader> *)stateVS;

  SAFE_RELEASE(stateVS);

  if(!ps)
    return empty;

  D3D11RenderState *rs = m_pImmediateContext->GetCurrentPipelineState();

  DXBCFile *dxbc = ps->GetDXBC();

  if(!dxbc)
    return empty;

  dxbc->GetDisassembly();

  DXBCFile *prevdxbc = NULL;

  if(prevdxbc == NULL && gs != NULL)
    prevdxbc = gs->GetDXBC();
  if(prevdxbc == NULL && ds != NULL)
    prevdxbc = ds->GetDXBC();
  if(prevdxbc == NULL && vs != NULL)
    prevdxbc = vs->GetDXBC();

  vector<DataOutput> initialValues;

  string extractHlsl = "struct PSInput\n{\n";

  int structureStride = 0;

  if(dxbc->m_InputSig.empty())
  {
    extractHlsl += "float4 input_dummy : SV_Position;\n";

    initialValues.push_back(DataOutput(-1, 0, 4, ShaderBuiltin::Undefined, true));

    structureStride += 4;
  }

  vector<string> floatInputs;
  vector<pair<string, pair<uint32_t, uint32_t> > >
      arrays;    // name, pair<start semantic index, end semantic index>

  uint32_t nextreg = 0;

  for(size_t i = 0; i < dxbc->m_InputSig.size(); i++)
  {
    extractHlsl += "  ";

    bool included = true;

    // handled specially to account for SV_ ordering
    if(dxbc->m_InputSig[i].systemValue == ShaderBuiltin::PrimitiveIndex ||
       dxbc->m_InputSig[i].systemValue == ShaderBuiltin::MSAACoverage ||
       dxbc->m_InputSig[i].systemValue == ShaderBuiltin::IsFrontFace ||
       dxbc->m_InputSig[i].systemValue == ShaderBuiltin::MSAASampleIndex)
    {
      extractHlsl += "//";
      included = false;
    }

    bool arrayElem = false;

    for(size_t a = 0; a < arrays.size(); a++)
    {
      if(dxbc->m_InputSig[i].semanticName == arrays[a].first &&
         arrays[a].second.first <= dxbc->m_InputSig[i].semanticIndex &&
         arrays[a].second.second >= dxbc->m_InputSig[i].semanticIndex)
      {
        extractHlsl += "//";
        included = false;
        arrayElem = true;
      }
    }

    int missingreg = int(dxbc->m_InputSig[i].regIndex) - int(nextreg);

    // fill in holes from output sig of previous shader if possible, to try and
    // ensure the same register order
    for(int dummy = 0; dummy < missingreg; dummy++)
    {
      bool filled = false;

      if(prevdxbc)
      {
        for(size_t os = 0; os < prevdxbc->m_OutputSig.size(); os++)
        {
          if(prevdxbc->m_OutputSig[os].regIndex == nextreg + dummy)
          {
            filled = true;

            if(prevdxbc->m_OutputSig[os].compType == CompType::Float)
              extractHlsl += "float";
            else if(prevdxbc->m_OutputSig[os].compType == CompType::SInt)
              extractHlsl += "int";
            else if(prevdxbc->m_OutputSig[os].compType == CompType::UInt)
              extractHlsl += "uint";
            else
              RDCERR("Unexpected input signature type: %d", prevdxbc->m_OutputSig[os].compType);

            int numCols = (prevdxbc->m_OutputSig[os].regChannelMask & 0x1 ? 1 : 0) +
                          (prevdxbc->m_OutputSig[os].regChannelMask & 0x2 ? 1 : 0) +
                          (prevdxbc->m_OutputSig[os].regChannelMask & 0x4 ? 1 : 0) +
                          (prevdxbc->m_OutputSig[os].regChannelMask & 0x8 ? 1 : 0);

            structureStride += 4 * numCols;

            initialValues.push_back(DataOutput(-1, 0, numCols, ShaderBuiltin::Undefined, true));

            std::string name = prevdxbc->m_OutputSig[os].semanticIdxName;

            extractHlsl += ToStr((uint32_t)numCols) + " input_" + name + " : " + name + ";\n";
          }
        }
      }

      if(!filled)
      {
        string dummy_reg = "dummy_register";
        dummy_reg += ToStr((uint32_t)nextreg + dummy);
        extractHlsl += "float4 var_" + dummy_reg + " : semantic_" + dummy_reg + ";\n";

        initialValues.push_back(DataOutput(-1, 0, 4, ShaderBuiltin::Undefined, true));

        structureStride += 4 * sizeof(float);
      }
    }

    nextreg = dxbc->m_InputSig[i].regIndex + 1;

    if(dxbc->m_InputSig[i].compType == CompType::Float)
      extractHlsl += "float";
    else if(dxbc->m_InputSig[i].compType == CompType::SInt)
      extractHlsl += "int";
    else if(dxbc->m_InputSig[i].compType == CompType::UInt)
      extractHlsl += "uint";
    else
      RDCERR("Unexpected input signature type: %d", dxbc->m_InputSig[i].compType);

    int numCols = (dxbc->m_InputSig[i].regChannelMask & 0x1 ? 1 : 0) +
                  (dxbc->m_InputSig[i].regChannelMask & 0x2 ? 1 : 0) +
                  (dxbc->m_InputSig[i].regChannelMask & 0x4 ? 1 : 0) +
                  (dxbc->m_InputSig[i].regChannelMask & 0x8 ? 1 : 0);

    if(included)
      structureStride += 4 * numCols;

    std::string name = dxbc->m_InputSig[i].semanticIdxName;

    // arrays of interpolators are handled really weirdly. They use cbuffer
    // packing rules where each new value is in a new register (rather than
    // e.g. 2 x float2 in a single register), but that's pointless because
    // you can't dynamically index into input registers.
    // If we declare those elements as a non-array, the float2s or floats
    // will be packed into registers and won't match up to the previous
    // shader.
    // HOWEVER to add an extra bit of fun, fxc will happily pack other
    // parameters not in the array into spare parts of the registers.
    //
    // So I think the upshot is that we can detect arrays reliably by
    // whenever we encounter a float or float2 at the start of a register,
    // search forward to see if the next register has an element that is the
    // same semantic name and one higher semantic index. If so, there's an
    // array, so keep searching to enumerate its length.
    // I think this should be safe if the packing just happens to place those
    // registers together.

    int arrayLength = 0;

    if(included && numCols <= 2 && dxbc->m_InputSig[i].regChannelMask <= 0x3)
    {
      uint32_t nextIdx = dxbc->m_InputSig[i].semanticIndex + 1;

      for(size_t j = i + 1; j < dxbc->m_InputSig.size(); j++)
      {
        // if we've found the 'next' semantic
        if(dxbc->m_InputSig[i].semanticName == dxbc->m_InputSig[j].semanticName &&
           nextIdx == dxbc->m_InputSig[j].semanticIndex)
        {
          int jNumCols = (dxbc->m_InputSig[i].regChannelMask & 0x1 ? 1 : 0) +
                         (dxbc->m_InputSig[i].regChannelMask & 0x2 ? 1 : 0) +
                         (dxbc->m_InputSig[i].regChannelMask & 0x4 ? 1 : 0) +
                         (dxbc->m_InputSig[i].regChannelMask & 0x8 ? 1 : 0);

          // if it's the same size, and it's at the start of the next register
          if(jNumCols == numCols && dxbc->m_InputSig[j].regChannelMask <= 0x3)
          {
            if(arrayLength == 0)
              arrayLength = 2;
            else
              arrayLength++;

            // continue searching now
            nextIdx++;
            j = i + 1;
            continue;
          }
        }
      }

      if(arrayLength > 0)
        arrays.push_back(
            std::make_pair(dxbc->m_InputSig[i].semanticName,
                           std::make_pair(dxbc->m_InputSig[i].semanticIndex, nextIdx - 1)));
    }

    // as another side effect of the above, an element declared as a 1-length array won't be
    // detected but it WILL be put in its own register (not packed together), so detect this
    // case too.
    // Note we have to search *backwards* because we need to know if this register should have
    // been packed into the previous register, but wasn't. float/float2 can be packed after an
    // array just fine.
    if(included && i > 0 && arrayLength == 0 && numCols <= 2 &&
       dxbc->m_InputSig[i].regChannelMask <= 0x3)
    {
      const SigParameter &prev = dxbc->m_InputSig[i - 1];

      if(prev.regIndex != dxbc->m_InputSig[i].regIndex && prev.compCount <= 2 &&
         prev.regChannelMask <= 0x3)
        arrayLength = 1;
    }

    // The compiler is also really annoying and will go to great lengths to rearrange elements
    // and screw up our declaration, to pack things together. E.g.:
    // float2 a : TEXCOORD1;
    // float4 b : TEXCOORD2;
    // float4 c : TEXCOORD3;
    // float2 d : TEXCOORD4;
    // the compiler will move d up and pack it into the last two components of a.
    // To prevent this, we look forward and backward to check that we aren't expecting to pack
    // with anything, and if not then we just make it a 1-length array to ensure no packing.
    // Note the regChannelMask & 0x1 means it is using .x, so it's not the tail-end of a pack
    if(included && arrayLength == 0 && numCols <= 2 && (dxbc->m_InputSig[i].regChannelMask & 0x1))
    {
      if(i == dxbc->m_InputSig.size() - 1)
      {
        // the last element is never packed
        arrayLength = 1;
      }
      else
      {
        // if the next reg is using .x, it wasn't packed with us
        if(dxbc->m_InputSig[i + 1].regChannelMask & 0x1)
          arrayLength = 1;
      }
    }

    extractHlsl += ToStr((uint32_t)numCols) + " input_" + name;
    if(arrayLength > 0)
      extractHlsl += "[" + ToStr(arrayLength) + "]";
    extractHlsl += " : " + name;

    if(included && dxbc->m_InputSig[i].compType == CompType::Float)
    {
      if(arrayLength == 0)
      {
        floatInputs.push_back("input_" + name);
      }
      else
      {
        for(int a = 0; a < arrayLength; a++)
          floatInputs.push_back("input_" + name + "[" + ToStr(a) + "]");
      }
    }

    extractHlsl += ";\n";

    int firstElem = dxbc->m_InputSig[i].regChannelMask & 0x1
                        ? 0
                        : dxbc->m_InputSig[i].regChannelMask & 0x2
                              ? 1
                              : dxbc->m_InputSig[i].regChannelMask & 0x4
                                    ? 2
                                    : dxbc->m_InputSig[i].regChannelMask & 0x8 ? 3 : -1;

    // arrays get added all at once (because in the struct data, they are contiguous even if
    // in the input signature they're not).
    if(!arrayElem)
    {
      if(arrayLength == 0)
      {
        initialValues.push_back(DataOutput(dxbc->m_InputSig[i].regIndex, firstElem, numCols,
                                           dxbc->m_InputSig[i].systemValue, included));
      }
      else
      {
        for(int a = 0; a < arrayLength; a++)
        {
          initialValues.push_back(DataOutput(dxbc->m_InputSig[i].regIndex + a, firstElem, numCols,
                                             dxbc->m_InputSig[i].systemValue, included));
        }
      }
    }
  }

  extractHlsl += "};\n\n";

  uint32_t overdrawLevels = 100;    // maximum number of overdraw levels

  uint32_t uavslot = 0;

  ID3D11DepthStencilView *depthView = NULL;
  ID3D11RenderTargetView *rtView = NULL;
  // preserve at least one render target and/or the depth view, so that
  // we have the right multisample level on output either way
  m_pImmediateContext->OMGetRenderTargets(1, &rtView, &depthView);
  if(rtView != NULL)
    uavslot = 1;

  extractHlsl +=
      "struct PSInitialData { uint hit; float3 pos; uint prim; uint fface; uint sample; uint "
      "covge; float derivValid; PSInput IN; PSInput INddx; PSInput INddy; PSInput INddxfine; "
      "PSInput INddyfine; };\n\n";
  extractHlsl +=
      "RWStructuredBuffer<PSInitialData> PSInitialBuffer : register(u" + ToStr(uavslot) + ");\n\n";
  extractHlsl +=
      "void ExtractInputsPS(PSInput IN, float4 debug_pixelPos : SV_Position, uint prim : "
      "SV_PrimitiveID, uint sample : SV_SampleIndex, uint covge : SV_Coverage, bool fface : "
      "SV_IsFrontFace)\n{\n";
  extractHlsl += "  uint idx = " + ToStr(overdrawLevels) + ";\n";
  extractHlsl += "  if(abs(debug_pixelPos.x - " + ToStr(x) +
                 ".5) < 0.5f && abs(debug_pixelPos.y - " + ToStr(y) + ".5) < 0.5f)\n";
  extractHlsl += "    InterlockedAdd(PSInitialBuffer[0].hit, 1, idx);\n\n";
  extractHlsl += "  idx = min(idx, " + ToStr(overdrawLevels) + ");\n\n";
  extractHlsl += "  PSInitialBuffer[idx].pos = debug_pixelPos.xyz;\n";
  extractHlsl += "  PSInitialBuffer[idx].prim = prim;\n";
  extractHlsl += "  PSInitialBuffer[idx].fface = fface;\n";
  extractHlsl += "  PSInitialBuffer[idx].covge = covge;\n";
  extractHlsl += "  PSInitialBuffer[idx].sample = sample;\n";
  extractHlsl += "  PSInitialBuffer[idx].IN = IN;\n";
  extractHlsl += "  PSInitialBuffer[idx].derivValid = ddx(debug_pixelPos.x);\n";
  extractHlsl += "  PSInitialBuffer[idx].INddx = (PSInput)0;\n";
  extractHlsl += "  PSInitialBuffer[idx].INddy = (PSInput)0;\n";
  extractHlsl += "  PSInitialBuffer[idx].INddxfine = (PSInput)0;\n";
  extractHlsl += "  PSInitialBuffer[idx].INddyfine = (PSInput)0;\n";

  for(size_t i = 0; i < floatInputs.size(); i++)
  {
    const string &name = floatInputs[i];
    extractHlsl += "  PSInitialBuffer[idx].INddx." + name + " = ddx(IN." + name + ");\n";
    extractHlsl += "  PSInitialBuffer[idx].INddy." + name + " = ddy(IN." + name + ");\n";
    extractHlsl += "  PSInitialBuffer[idx].INddxfine." + name + " = ddx_fine(IN." + name + ");\n";
    extractHlsl += "  PSInitialBuffer[idx].INddyfine." + name + " = ddy_fine(IN." + name + ");\n";
  }
  extractHlsl += "\n}";

  ID3D11PixelShader *extract =
      m_pDevice->GetShaderCache()->MakePShader(extractHlsl.c_str(), "ExtractInputsPS", "ps_5_0");

  uint32_t structStride = sizeof(uint32_t)       // uint hit;
                          + sizeof(float) * 3    // float3 pos;
                          + sizeof(uint32_t)     // uint prim;
                          + sizeof(uint32_t)     // uint fface;
                          + sizeof(uint32_t)     // uint sample;
                          + sizeof(uint32_t)     // uint covge;
                          + sizeof(float)        // float derivValid;
                          +
                          structureStride * 5;    // PSInput IN, INddx, INddy, INddxfine, INddyfine;

  HRESULT hr = S_OK;

  D3D11_BUFFER_DESC bdesc;
  bdesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bdesc.CPUAccessFlags = 0;
  bdesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bdesc.Usage = D3D11_USAGE_DEFAULT;
  bdesc.StructureByteStride = structStride;
  bdesc.ByteWidth = bdesc.StructureByteStride * (overdrawLevels + 1);

  ID3D11Buffer *initialBuf = NULL;
  hr = m_pDevice->CreateBuffer(&bdesc, NULL, &initialBuf);

  if(FAILED(hr))
  {
    RDCERR("Failed to create buffer HRESULT: %s", ToStr(hr).c_str());
    return empty;
  }

  bdesc.BindFlags = 0;
  bdesc.MiscFlags = 0;
  bdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  bdesc.Usage = D3D11_USAGE_STAGING;
  bdesc.StructureByteStride = 0;

  ID3D11Buffer *stageBuf = NULL;
  hr = m_pDevice->CreateBuffer(&bdesc, NULL, &stageBuf);

  if(FAILED(hr))
  {
    RDCERR("Failed to create buffer HRESULT: %s", ToStr(hr).c_str());
    return empty;
  }

  D3D11_UNORDERED_ACCESS_VIEW_DESC uavdesc;
  uavdesc.Format = DXGI_FORMAT_UNKNOWN;
  uavdesc.Buffer.FirstElement = 0;
  uavdesc.Buffer.Flags = 0;
  uavdesc.Buffer.NumElements = overdrawLevels + 1;
  uavdesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;

  ID3D11UnorderedAccessView *initialUAV = NULL;
  hr = m_pDevice->CreateUnorderedAccessView(initialBuf, &uavdesc, &initialUAV);

  if(FAILED(hr))
  {
    RDCERR("Failed to create buffer HRESULT: %s", ToStr(hr).c_str());
    return empty;
  }

  UINT zero = 0;
  m_pImmediateContext->ClearUnorderedAccessViewUint(initialUAV, &zero);

  UINT count = (UINT)-1;
  m_pImmediateContext->OMSetRenderTargetsAndUnorderedAccessViews(uavslot, &rtView, depthView,
                                                                 uavslot, 1, &initialUAV, &count);
  m_pImmediateContext->PSSetShader(extract, NULL, 0);

  SAFE_RELEASE(rtView);
  SAFE_RELEASE(depthView);

  {
    D3D11MarkerRegion initState("Replaying event for initial states");

    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);

    m_pImmediateContext->CopyResource(stageBuf, initialBuf);
  }

  D3D11_MAPPED_SUBRESOURCE mapped;
  hr = m_pImmediateContext->Map(stageBuf, 0, D3D11_MAP_READ, 0, &mapped);

  if(FAILED(hr))
  {
    RDCERR("Failed to map stage buff HRESULT: %s", ToStr(hr).c_str());
    return empty;
  }

  byte *initialData = new byte[bdesc.ByteWidth];
  memcpy(initialData, mapped.pData, bdesc.ByteWidth);

  m_pImmediateContext->Unmap(stageBuf, 0);

  SAFE_RELEASE(initialUAV);
  SAFE_RELEASE(initialBuf);
  SAFE_RELEASE(stageBuf);

  SAFE_RELEASE(extract);

  DebugHit *buf = (DebugHit *)initialData;

  D3D11MarkerRegion::Set(StringFormat::Fmt("Got %u hits", buf[0].numHits));

  if(buf[0].numHits == 0)
  {
    RDCLOG("No hit for this event");
    SAFE_DELETE_ARRAY(initialData);
    return empty;
  }

  // if we encounter multiple hits at our destination pixel co-ord (or any other) we
  // check to see if a specific primitive was requested (via primitive parameter not
  // being set to ~0U). If it was, debug that pixel, otherwise do a best-estimate
  // of which fragment was the last to successfully depth test and debug that, just by
  // checking if the depth test is ordered and picking the final fragment in the series

  // our debugging quad. Order is TL, TR, BL, BR
  State quad[4];

  // figure out the TL pixel's coords. Assume even top left (towards 0,0)
  // this isn't spec'd but is a reasonable assumption.
  int xTL = x & (~1);
  int yTL = y & (~1);

  // get the index of our desired pixel
  int destIdx = (x - xTL) + 2 * (y - yTL);

  bytebuf cbufData[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];

  for(int i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
    if(rs->PS.ConstantBuffers[i])
      GetDebugManager()->GetBufferData(rs->PS.ConstantBuffers[i],
                                       rs->PS.CBOffsets[i] * sizeof(Vec4f), 0, cbufData[i]);

  D3D11_COMPARISON_FUNC depthFunc = D3D11_COMPARISON_LESS;

  if(rs->OM.DepthStencilState)
  {
    D3D11_DEPTH_STENCIL_DESC desc;
    rs->OM.DepthStencilState->GetDesc(&desc);
    depthFunc = desc.DepthFunc;
  }

  DebugHit *winner = NULL;

  if(sample == ~0U)
    sample = 0;

  if(primitive != ~0U)
  {
    for(size_t i = 0; i < buf[0].numHits && i < overdrawLevels; i++)
    {
      DebugHit *hit = (DebugHit *)(initialData + i * structStride);

      if(hit->primitive == primitive && hit->sample == sample)
        winner = hit;
    }
  }

  if(winner == NULL)
  {
    for(size_t i = 0; i < buf[0].numHits && i < overdrawLevels; i++)
    {
      DebugHit *hit = (DebugHit *)(initialData + i * structStride);

      if(winner == NULL || (winner->sample != sample && hit->sample == sample) ||
         depthFunc == D3D11_COMPARISON_ALWAYS || depthFunc == D3D11_COMPARISON_NEVER ||
         depthFunc == D3D11_COMPARISON_NOT_EQUAL || depthFunc == D3D11_COMPARISON_EQUAL)
      {
        winner = hit;
        continue;
      }

      if((depthFunc == D3D11_COMPARISON_LESS && hit->depth < winner->depth) ||
         (depthFunc == D3D11_COMPARISON_LESS_EQUAL && hit->depth <= winner->depth) ||
         (depthFunc == D3D11_COMPARISON_GREATER && hit->depth > winner->depth) ||
         (depthFunc == D3D11_COMPARISON_GREATER_EQUAL && hit->depth >= winner->depth))
      {
        if(hit->sample == sample)
          winner = hit;
      }
    }
  }

  if(winner == NULL)
  {
    RDCLOG("Couldn't find any pixels that passed depth test at target co-ordinates");
    SAFE_DELETE_ARRAY(initialData);
    return empty;
  }

  ShaderDebugTrace traces[4];

  GlobalState global;
  GetDebugManager()->CreateShaderGlobalState(global, dxbc, rs->OM.UAVStartSlot, rs->OM.UAVs,
                                             rs->PS.SRVs);

  {
    DebugHit *hit = winner;

    State initialState =
        GetDebugManager()->CreateShaderDebugState(traces[destIdx], destIdx, dxbc, cbufData);

    rdcarray<ShaderVariable> &ins = traces[destIdx].inputs;
    if(!ins.empty() && ins.back().name == "vCoverage")
      ins.back().value.u.x = hit->coverage;

    initialState.semantics.coverage = hit->coverage;
    initialState.semantics.primID = hit->primitive;
    initialState.semantics.isFrontFace = hit->isFrontFace;

    uint32_t *data = &hit->rawdata;

    float *ddx = (float *)data;

    // ddx(SV_Position.x) MUST be 1.0
    if(*ddx != 1.0f)
    {
      RDCERR("Derivatives invalid");
      SAFE_DELETE_ARRAY(initialData);
      return empty;
    }

    data++;

    for(size_t i = 0; i < initialValues.size(); i++)
    {
      int32_t *rawout = NULL;

      if(initialValues[i].reg >= 0)
      {
        ShaderVariable &invar = traces[destIdx].inputs[initialValues[i].reg];

        if(initialValues[i].sysattribute == ShaderBuiltin::PrimitiveIndex)
        {
          invar.value.u.x = hit->primitive;
        }
        else if(initialValues[i].sysattribute == ShaderBuiltin::MSAASampleIndex)
        {
          invar.value.u.x = hit->sample;
        }
        else if(initialValues[i].sysattribute == ShaderBuiltin::MSAACoverage)
        {
          invar.value.u.x = hit->coverage;
        }
        else if(initialValues[i].sysattribute == ShaderBuiltin::IsFrontFace)
        {
          invar.value.u.x = hit->isFrontFace ? ~0U : 0;
        }
        else
        {
          rawout = &invar.value.iv[initialValues[i].elem];

          memcpy(rawout, data, initialValues[i].numwords * 4);
        }
      }

      if(initialValues[i].included)
        data += initialValues[i].numwords;
    }

    for(int i = 0; i < 4; i++)
    {
      if(i != destIdx)
        traces[i] = traces[destIdx];
      quad[i] = initialState;
      quad[i].SetTrace(i, &traces[i]);
      if(i != destIdx)
        quad[i].SetHelper();
    }

    // We make the assumption that the coarse derivatives are
    // generated from (0,0) in the quad, and fine derivatives
    // are generated from the destination index and its
    // neighbours in X and Y.
    // This isn't spec'd but we must assume something and this
    // will hopefully get us closest to reproducing actual
    // results.
    //
    // For debugging, we need members of the quad to be able
    // to generate coarse and fine derivatives.
    //
    // For (0,0) we only need the coarse derivatives to get
    // our neighbours (1,0) and (0,1) which will give us
    // coarse and fine derivatives being identical.
    //
    // For the others we will need to use a combination of
    // coarse and fine derivatives to get the diagonal element
    // in the quad. E.g. for (1,1):
    //
    // (1,0) = (1,1) - ddx_fine
    // (0,1) = (1,1) - ddy_fine
    // (0,0) = (1,1) - ddy_fine - ddx_coarse
    //
    // This only works if coarse and fine are calculated as we
    // are assuming.

    ddx = (float *)data;

    for(size_t i = 0; i < initialValues.size(); i++)
    {
      if(!initialValues[i].included)
        continue;

      if(initialValues[i].reg >= 0)
      {
        if(destIdx == 0)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[1].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddx[w];
            traces[3].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddx[w];
          }
        }
        else if(destIdx == 1)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[0].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddx[w];
            traces[2].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddx[w];
          }
        }
        else if(destIdx == 2)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[1].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddx[w];
          }
        }
        else if(destIdx == 3)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[0].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddx[w];
          }
        }
      }

      ddx += initialValues[i].numwords;
    }

    float *ddy = ddx;

    for(size_t i = 0; i < initialValues.size(); i++)
    {
      if(!initialValues[i].included)
        continue;

      if(initialValues[i].reg >= 0)
      {
        if(destIdx == 0)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[2].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddy[w];
            traces[3].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddy[w];
          }
        }
        else if(destIdx == 1)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[2].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddy[w];
          }
        }
        else if(destIdx == 2)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[0].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddy[w];
            traces[1].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddy[w];
          }
        }
      }

      ddy += initialValues[i].numwords;
    }

    float *ddxfine = ddy;

    for(size_t i = 0; i < initialValues.size(); i++)
    {
      if(!initialValues[i].included)
        continue;

      if(initialValues[i].reg >= 0)
      {
        if(destIdx == 2)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[3].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddxfine[w];
          }
        }
        else if(destIdx == 3)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[2].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddxfine[w];
          }
        }
      }

      ddxfine += initialValues[i].numwords;
    }

    float *ddyfine = ddxfine;

    for(size_t i = 0; i < initialValues.size(); i++)
    {
      if(!initialValues[i].included)
        continue;

      if(initialValues[i].reg >= 0)
      {
        if(destIdx == 1)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[3].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] += ddyfine[w];
          }
        }
        else if(destIdx == 3)
        {
          for(int w = 0; w < initialValues[i].numwords; w++)
          {
            traces[1].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddyfine[w];
            traces[0].inputs[initialValues[i].reg].value.fv[initialValues[i].elem + w] -= ddyfine[w];
          }
        }
      }

      ddyfine += initialValues[i].numwords;
    }
  }

  SAFE_DELETE_ARRAY(initialData);

  vector<ShaderDebugState> states;

  states.push_back((State)quad[destIdx]);

  // ping pong between so that we can have 'current' quad to update into new one
  State quad2[4];

  State *curquad = quad;
  State *newquad = quad2;

  // marks any threads stalled waiting for others to catch up
  bool activeMask[4] = {true, true, true, true};

  int cycleCounter = 0;

  D3D11MarkerRegion simloop("Simulation Loop");

  // simulate lockstep until all threads are finished
  bool finished = true;
  do
  {
    for(size_t i = 0; i < 4; i++)
    {
      if(activeMask[i])
        newquad[i] = curquad[i].GetNext(global, curquad);
      else
        newquad[i] = curquad[i];
    }

    State *a = curquad;
    curquad = newquad;
    newquad = a;

    // if our destination quad is paused don't record multiple identical states.
    if(activeMask[destIdx])
      states.push_back((State)curquad[destIdx]);

    // we need to make sure that control flow which converges stays in lockstep so that
    // derivatives are still valid. While diverged, we don't have to keep threads in lockstep
    // since using derivatives is invalid.

    // Threads diverge either in ifs, loops, or switches. Due to the nature of the bytecode,
    // all threads *must* pass through the same exit instruction for each, there's no jumping
    // around with gotos. Note also for the same reason, the only time threads are on earlier
    // instructions is if they are still catching up to a thread that has exited the control
    // flow.

    // So the scheme is as follows:
    // * If all threads have the same nextInstruction, just continue we are still in lockstep.
    // * If threads are out of lockstep, find any thread which has nextInstruction pointing
    //   immediately *after* an ENDIF, ENDLOOP or ENDSWITCH. Pointing directly at one is not
    //   an indication the thread is done, as the next step for an ENDLOOP will jump back to
    //   the matching LOOP and continue iterating.
    // * Pause any thread matching the above until all threads are pointing to the same
    //   instruction. By the assumption above, all threads will eventually pass through this
    //   terminating instruction so we just pause any other threads and don't do anything
    //   until the control flow has converged and we can continue stepping in lockstep.

    // mark all threads as active again.
    // if we've converged, or we were never diverged, this keeps everything ticking
    activeMask[0] = activeMask[1] = activeMask[2] = activeMask[3] = true;

    if(curquad[0].nextInstruction != curquad[1].nextInstruction ||
       curquad[0].nextInstruction != curquad[2].nextInstruction ||
       curquad[0].nextInstruction != curquad[3].nextInstruction)
    {
      // this isn't *perfect* but it will still eventually continue. We look for the most
      // advanced thread, and check to see if it's just finished a control flow. If it has
      // then we assume it's at the convergence point and wait for every other thread to
      // catch up, pausing any threads that reach the convergence point before others.

      // Note this might mean we don't have any threads paused even within divergent flow.
      // This is fine and all we care about is pausing to make sure threads don't run ahead
      // into code that should be lockstep. We don't care at all about what they do within
      // the code that is divergent.

      // The reason this isn't perfect is that the most advanced thread could be on an
      // inner loop or inner if, not the convergence point, and we could be pausing it
      // fruitlessly. Worse still - it could be on a branch none of the other threads will
      // take so they will never reach that exact instruction.
      // But we know that all threads will eventually go through the convergence point, so
      // even in that worst case if we didn't pick the right waiting point, another thread
      // will overtake and become the new most advanced thread and the previous waiting
      // thread will resume. So in this case we caused a thread to wait more than it should
      // have but that's not a big deal as it's within divergent flow so they don't have to
      // stay in lockstep. Also if all threads will eventually pass that point we picked,
      // we just waited to converge even in technically divergent code which is also
      // harmless.

      // Phew!

      uint32_t convergencePoint = 0;

      // find which thread is most advanced
      for(size_t i = 0; i < 4; i++)
        if(curquad[i].nextInstruction > convergencePoint)
          convergencePoint = curquad[i].nextInstruction;

      if(convergencePoint > 0)
      {
        OpcodeType op = dxbc->GetInstruction(convergencePoint - 1).operation;

        // if the most advnaced thread hasn't just finished control flow, then all
        // threads are still running, so don't converge
        if(op != OPCODE_ENDIF && op != OPCODE_ENDLOOP && op != OPCODE_ENDSWITCH)
          convergencePoint = 0;
      }

      // pause any threads at that instruction (could be none)
      for(size_t i = 0; i < 4; i++)
        if(curquad[i].nextInstruction == convergencePoint)
          activeMask[i] = false;
    }

    finished = curquad[destIdx].Finished();

    cycleCounter++;

    if(cycleCounter == SHADER_DEBUG_WARN_THRESHOLD)
    {
      if(PromptDebugTimeout(DXBC::TYPE_VERTEX, cycleCounter))
        break;
    }
  } while(!finished);

  traces[destIdx].states = states;

  return traces[destIdx];
}

ShaderDebugTrace D3D11Replay::DebugThread(uint32_t eventId, const uint32_t groupid[3],
                                          const uint32_t threadid[3])
{
  using namespace DXBC;
  using namespace ShaderDebug;

  D3D11MarkerRegion simloop(StringFormat::Fmt("DebugThread @ %u: [%u, %u, %u] (%u, %u, %u)",
                                              eventId, groupid[0], groupid[1], groupid[2],
                                              threadid[0], threadid[1], threadid[2]));

  ShaderDebugTrace empty;

  D3D11RenderStateTracker tracker(m_pImmediateContext);

  ID3D11ComputeShader *stateCS = NULL;
  m_pImmediateContext->CSGetShader(&stateCS, NULL, NULL);

  WrappedID3D11Shader<ID3D11ComputeShader> *cs = (WrappedID3D11Shader<ID3D11ComputeShader> *)stateCS;

  SAFE_RELEASE(stateCS);

  if(!cs)
    return empty;

  DXBCFile *dxbc = cs->GetDXBC();

  if(!dxbc)
    return empty;

  dxbc->GetDisassembly();

  D3D11RenderState *rs = m_pImmediateContext->GetCurrentPipelineState();

  bytebuf cbufData[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];

  for(int i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; i++)
    if(rs->CS.ConstantBuffers[i])
      GetDebugManager()->GetBufferData(rs->CS.ConstantBuffers[i],
                                       rs->CS.CBOffsets[i] * sizeof(Vec4f), 0, cbufData[i]);

  ShaderDebugTrace ret;

  GlobalState global;
  GetDebugManager()->CreateShaderGlobalState(global, dxbc, 0, rs->CSUAVs, rs->CS.SRVs);
  State initialState = GetDebugManager()->CreateShaderDebugState(ret, -1, dxbc, cbufData);

  for(int i = 0; i < 3; i++)
  {
    initialState.semantics.GroupID[i] = groupid[i];
    initialState.semantics.ThreadID[i] = threadid[i];
  }

  vector<ShaderDebugState> states;

  states.push_back((State)initialState);

  for(int cycleCounter = 0;; cycleCounter++)
  {
    if(initialState.Finished())
      break;

    initialState = initialState.GetNext(global, NULL);

    states.push_back((State)initialState);

    if(cycleCounter == SHADER_DEBUG_WARN_THRESHOLD)
    {
      if(PromptDebugTimeout(DXBC::TYPE_VERTEX, cycleCounter))
        break;
    }
  }

  ret.states = states;

  return ret;
}
