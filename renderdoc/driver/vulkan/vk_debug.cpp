/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
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

#include "vk_debug.h"
#include "vk_core.h"

#include "stb/stb_truetype.h"

// VKTODOMED should share this between shader and C++ - need #include support in glslang
struct displayuniforms
{
	Vec2f Position;
	float Scale;
	float HDRMul;

	Vec4f Channels;

	float RangeMinimum;
	float InverseRangeSize;
	float MipLevel;
	int   FlipY;

	Vec3f TextureResolutionPS;
	int   OutputDisplayFormat;

	Vec2f OutputRes;
	int   RawOutput;
	float Slice;

	int   SampleIdx;
	int   NumSamples;
	Vec2f Padding;
};

struct fontuniforms
{
	Vec2f TextPosition;
	float txtpadding;
	float TextSize;

	Vec2f CharacterSize;
	Vec2f FontScreenAspect;
};

struct genericuniforms
{
	Vec4f Offset;
	Vec4f Scale;
	Vec4f Color;
};
		
struct glyph
{
	Vec4f posdata;
	Vec4f uvdata;
};

struct glyphdata
{
	glyph glyphs[127-32];
};

struct stringdata
{
	uint32_t str[256][4];
};

void VulkanDebugManager::GPUBuffer::Create(WrappedVulkan *driver, VkDevice dev, VkDeviceSize size)
{
	const VkLayerDispatchTable *vt = ObjDisp(dev);

	VkBufferCreateInfo bufInfo = {
		VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL,
		size, VK_BUFFER_USAGE_GENERAL, 0,
		VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
	};

	VkResult vkr = vt->CreateBuffer(Unwrap(dev), &bufInfo, &buf);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), buf);

	VkMemoryRequirements mrq;
	vkr = vt->GetBufferMemoryRequirements(Unwrap(dev), Unwrap(buf), &mrq);
	RDCASSERT(vkr == VK_SUCCESS);

	// VKTODOMED maybe don't require host visible, and do map & copy?
	VkMemoryAllocInfo allocInfo = {
		VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO, NULL,
		size, driver->GetUploadMemoryIndex(mrq.memoryTypeBits),
	};

	vkr = vt->AllocMemory(Unwrap(dev), &allocInfo, &mem);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), mem);

	vkr = vt->BindBufferMemory(Unwrap(dev), Unwrap(buf), Unwrap(mem), 0);
	RDCASSERT(vkr == VK_SUCCESS);

	VkBufferViewCreateInfo bufviewInfo = {
		VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL,
		Unwrap(buf), VK_BUFFER_VIEW_TYPE_RAW,
		VK_FORMAT_UNDEFINED, 0, size,
	};

	vkr = vt->CreateBufferView(Unwrap(dev), &bufviewInfo, &view);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), view);
}

void VulkanDebugManager::GPUBuffer::Destroy(const VkLayerDispatchTable *vt, VkDevice dev)
{
	VkResult vkr = VK_SUCCESS;
	if(view != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyBufferView(Unwrap(dev), Unwrap(view));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(view);
		view = VK_NULL_HANDLE;
	}
	
	if(buf != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyBuffer(Unwrap(dev), Unwrap(buf));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(buf);
		buf = VK_NULL_HANDLE;
	}

	if(mem != VK_NULL_HANDLE)
	{
		vkr = vt->FreeMemory(Unwrap(dev), Unwrap(mem));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(mem);
		mem = VK_NULL_HANDLE;
	}
}

void *VulkanDebugManager::GPUBuffer::Map(const VkLayerDispatchTable *vt, VkDevice dev, VkDeviceSize offset, VkDeviceSize size)
{
	void *ptr = NULL;
	VkResult vkr = vt->MapMemory(Unwrap(dev), Unwrap(mem), offset, size, 0, (void **)&ptr);
	RDCASSERT(vkr == VK_SUCCESS);
	return ptr;
}

void VulkanDebugManager::GPUBuffer::Unmap(const VkLayerDispatchTable *vt, VkDevice dev)
{
	vt->UnmapMemory(Unwrap(dev), Unwrap(mem));
}

VulkanDebugManager::VulkanDebugManager(WrappedVulkan *driver, VkDevice dev)
{
	// VKTODOLOW needs tidy up - isn't scalable. Needs more classes like UBO above.

	m_DescriptorPool = VK_NULL_HANDLE;
	m_DynamicCBStateWhite = VK_NULL_HANDLE;
	m_DynamicRSState = VK_NULL_HANDLE;
	m_DynamicDSStateDisabled = VK_NULL_HANDLE;
	m_LinearSampler = VK_NULL_HANDLE;
	m_PointSampler = VK_NULL_HANDLE;

	m_CheckerboardDescSetLayout = VK_NULL_HANDLE;
	m_CheckerboardPipeLayout = VK_NULL_HANDLE;
	m_CheckerboardDescSet = VK_NULL_HANDLE;
	m_CheckerboardPipeline = VK_NULL_HANDLE;
	RDCEraseEl(m_CheckerboardUBO);

	m_TexDisplayDescSetLayout = VK_NULL_HANDLE;
	m_TexDisplayPipeLayout = VK_NULL_HANDLE;
	m_TexDisplayDescSet = VK_NULL_HANDLE;
	m_TexDisplayPipeline = VK_NULL_HANDLE;
	m_TexDisplayBlendPipeline = VK_NULL_HANDLE;
	RDCEraseEl(m_TexDisplayUBO);
			
	m_TextDescSetLayout = VK_NULL_HANDLE;
	m_TextPipeLayout = VK_NULL_HANDLE;
	m_TextDescSet = VK_NULL_HANDLE;
	m_TextPipeline = VK_NULL_HANDLE;
	RDCEraseEl(m_TextGeneralUBO);
	RDCEraseEl(m_TextGlyphUBO);
	RDCEraseEl(m_TextStringUBO);
	m_TextAtlas = VK_NULL_HANDLE;
	m_TextAtlasMem = VK_NULL_HANDLE;
	m_TextAtlasView = VK_NULL_HANDLE;

	m_GenericDescSetLayout = VK_NULL_HANDLE;
	m_GenericPipeLayout = VK_NULL_HANDLE;
	m_GenericDescSet = VK_NULL_HANDLE;
	m_GenericPipeline = VK_NULL_HANDLE;

	m_Device = dev;
	
	const VkLayerDispatchTable *vt = ObjDisp(dev);

	VkResult vkr = VK_SUCCESS;

	VkSamplerCreateInfo sampInfo = {
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL,
		VK_TEX_FILTER_LINEAR, VK_TEX_FILTER_LINEAR,
		VK_TEX_MIPMAP_MODE_LINEAR, 
		VK_TEX_ADDRESS_CLAMP, VK_TEX_ADDRESS_CLAMP, VK_TEX_ADDRESS_CLAMP,
		0.0f, // lod bias
		1.0f, // max aniso
		false, VK_COMPARE_OP_NEVER,
		0.0f, 0.0f, // min/max lod
		VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
	};

	vkr = vt->CreateSampler(Unwrap(dev), &sampInfo, &m_LinearSampler);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_LinearSampler);

	sampInfo.minFilter = VK_TEX_FILTER_NEAREST;
	sampInfo.magFilter = VK_TEX_FILTER_NEAREST;
	sampInfo.mipMode = VK_TEX_MIPMAP_MODE_NEAREST;

	vkr = vt->CreateSampler(Unwrap(dev), &sampInfo, &m_PointSampler);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_PointSampler);

	{
		// VKTODOLOW not sure if these stage flags VK_SHADER_STAGE_... work yet?
		VkDescriptorSetLayoutBinding layoutBinding[] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, }
		};

		VkDescriptorSetLayoutCreateInfo descsetLayoutInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
			ARRAY_COUNT(layoutBinding), &layoutBinding[0],
		};

		vkr = vt->CreateDescriptorSetLayout(Unwrap(dev), &descsetLayoutInfo, &m_CheckerboardDescSetLayout);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), m_CheckerboardDescSetLayout);

		// identical layout
		vkr = vt->CreateDescriptorSetLayout(Unwrap(dev), &descsetLayoutInfo, &m_GenericDescSetLayout);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), m_GenericDescSetLayout);
	}

	{
		VkDescriptorSetLayoutBinding layoutBinding[] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, NULL, }
		};

		VkDescriptorSetLayoutCreateInfo descsetLayoutInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
			ARRAY_COUNT(layoutBinding), &layoutBinding[0],
		};

		vkr = vt->CreateDescriptorSetLayout(Unwrap(dev), &descsetLayoutInfo, &m_TexDisplayDescSetLayout);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), m_TexDisplayDescSetLayout);
	}

	{
		VkDescriptorSetLayoutBinding layoutBinding[] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, NULL, }
		};

		VkDescriptorSetLayoutCreateInfo descsetLayoutInfo = {
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
			ARRAY_COUNT(layoutBinding), &layoutBinding[0],
		};

		vkr = vt->CreateDescriptorSetLayout(Unwrap(dev), &descsetLayoutInfo, &m_TextDescSetLayout);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), m_TextDescSetLayout);
	}

	VkPipelineLayoutCreateInfo pipeLayoutInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL,
		1, UnwrapPtr(m_TexDisplayDescSetLayout),
		0, NULL, // push constant ranges
	};
	
	vkr = vt->CreatePipelineLayout(Unwrap(dev), &pipeLayoutInfo, &m_TexDisplayPipeLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_TexDisplayPipeLayout);

	pipeLayoutInfo.pSetLayouts = UnwrapPtr(m_CheckerboardDescSetLayout);
	
	vkr = vt->CreatePipelineLayout(Unwrap(dev), &pipeLayoutInfo, &m_CheckerboardPipeLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_CheckerboardPipeLayout);

	pipeLayoutInfo.pSetLayouts = UnwrapPtr(m_TextDescSetLayout);
	
	vkr = vt->CreatePipelineLayout(Unwrap(dev), &pipeLayoutInfo, &m_TextPipeLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_TextPipeLayout);

	pipeLayoutInfo.pSetLayouts = UnwrapPtr(m_GenericDescSetLayout);
	
	vkr = vt->CreatePipelineLayout(Unwrap(dev), &pipeLayoutInfo, &m_GenericPipeLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_GenericPipeLayout);

	VkDescriptorTypeCount descPoolTypes[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024, },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024, },
	};
	
	VkDescriptorPoolCreateInfo descpoolInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
		ARRAY_COUNT(descPoolTypes), &descPoolTypes[0],
	};
	
	vkr = vt->CreateDescriptorPool(Unwrap(dev), VK_DESCRIPTOR_POOL_USAGE_ONE_SHOT, 4, &descpoolInfo, &m_DescriptorPool);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_DescriptorPool);
	
	uint32_t count;
	vkr = vt->AllocDescriptorSets(Unwrap(dev), Unwrap(m_DescriptorPool), VK_DESCRIPTOR_SET_USAGE_STATIC, 1,
		UnwrapPtr(m_CheckerboardDescSetLayout), &m_CheckerboardDescSet, &count);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_CheckerboardDescSet);
	
	vkr = vt->AllocDescriptorSets(Unwrap(dev), Unwrap(m_DescriptorPool), VK_DESCRIPTOR_SET_USAGE_STATIC, 1,
		UnwrapPtr(m_TexDisplayDescSetLayout), &m_TexDisplayDescSet, &count);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_TexDisplayDescSet);
	
	vkr = vt->AllocDescriptorSets(Unwrap(dev), Unwrap(m_DescriptorPool), VK_DESCRIPTOR_SET_USAGE_STATIC, 1,
		UnwrapPtr(m_TextDescSetLayout), &m_TextDescSet, &count);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_TextDescSet);
	
	vkr = vt->AllocDescriptorSets(Unwrap(dev), Unwrap(m_DescriptorPool), VK_DESCRIPTOR_SET_USAGE_STATIC, 1,
		UnwrapPtr(m_GenericDescSetLayout), &m_GenericDescSet, &count);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_GenericDescSet);

	m_GenericUBO.Create(driver, dev, 128);
	RDCCOMPILE_ASSERT(sizeof(genericuniforms) <= 128, "outline strip VBO size");

	{
		float data[] = {
			0.0f, -1.0f, 0.0f, 1.0f,
			1.0f, -1.0f, 0.0f, 1.0f,

			1.0f, -1.0f, 0.0f, 1.0f,
			1.0f,  0.0f, 0.0f, 1.0f,

			1.0f,  0.0f, 0.0f, 1.0f,
			0.0f,  0.0f, 0.0f, 1.0f,

			0.0f,  0.1f, 0.0f, 1.0f,
			0.0f, -1.0f, 0.0f, 1.0f,
		};
		
		m_OutlineStripVBO.Create(driver, dev, 128);
		RDCCOMPILE_ASSERT(sizeof(data) <= 128, "outline strip VBO size");
		
		float *mapped = (float *)m_OutlineStripVBO.Map(vt, dev);

		memcpy(mapped, data, sizeof(data));

		m_OutlineStripVBO.Unmap(vt, dev);
	}

	m_CheckerboardUBO.Create(driver, dev, 128);
	m_TexDisplayUBO.Create(driver, dev, 128);

	RDCCOMPILE_ASSERT(sizeof(displayuniforms) <= 128, "tex display size");
		
	m_TextGeneralUBO.Create(driver, dev, 128);
	RDCCOMPILE_ASSERT(sizeof(fontuniforms) <= 128, "font uniforms size");

	m_TextStringUBO.Create(driver, dev, 4096);
	RDCCOMPILE_ASSERT(sizeof(stringdata) <= 4096, "font uniforms size");

	VkDynamicRasterStateCreateInfo rsInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_RASTER_STATE_CREATE_INFO, NULL,
		0.0f, 0.0f, 0.0f, 1.0f,
	};

	vkr = vt->CreateDynamicRasterState(Unwrap(dev), &rsInfo, &m_DynamicRSState);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_DynamicRSState);
	
	VkDynamicColorBlendStateCreateInfo cbInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_COLOR_BLEND_STATE_CREATE_INFO, NULL,
		{ 1.0f, 1.0f, 1.0f, 1.0f },
	};

	vkr = vt->CreateDynamicColorBlendState(Unwrap(dev), &cbInfo, &m_DynamicCBStateWhite);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_DynamicCBStateWhite);
	
	VkDynamicDepthStencilStateCreateInfo dsInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_DEPTH_STENCIL_STATE_CREATE_INFO, NULL,
		0.0f, 1.0f, 0xff, 0xff, 0, 0,
	};

	vkr = vt->CreateDynamicDepthStencilState(Unwrap(dev), &dsInfo, &m_DynamicDSStateDisabled);
	RDCASSERT(vkr == VK_SUCCESS);

	VKMGR()->WrapResource(Unwrap(dev), m_DynamicDSStateDisabled);
	
	string shaderSources[] = {
		GetEmbeddedResource(blitvs_spv),
		GetEmbeddedResource(checkerboardfs_spv),
		GetEmbeddedResource(texdisplayfs_spv),
		GetEmbeddedResource(textvs_spv),
		GetEmbeddedResource(textfs_spv),
		GetEmbeddedResource(genericvs_spv),
		GetEmbeddedResource(genericfs_spv),
	};
	
	enum shaderIdx
	{
		BLITVS,
		CHECKERBOARDFS,
		TEXDISPLAYFS,
		TEXTVS,
		TEXTFS,
		GENERICVS,
		GENERICFS,
	};

	VkShaderModule module[ARRAY_COUNT(shaderSources)];
	VkShader shader[ARRAY_COUNT(shaderSources)];
	
	for(size_t i=0; i < ARRAY_COUNT(module); i++)
	{
		VkShaderModuleCreateInfo modinfo = {
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL,
			shaderSources[i].size(), (void *)&shaderSources[i][0], 0,
		};

		vkr = vt->CreateShaderModule(Unwrap(dev), &modinfo, &module[i]);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), module[i]);

		VkShaderCreateInfo shadinfo = {
			VK_STRUCTURE_TYPE_SHADER_CREATE_INFO, NULL,
			Unwrap(module[i]), "main", 0,
		};

		vkr = vt->CreateShader(Unwrap(dev), &shadinfo, &shader[i]);
		RDCASSERT(vkr == VK_SUCCESS);

		VKMGR()->WrapResource(Unwrap(dev), shader[i]);
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, VK_SHADER_STAGE_VERTEX, VK_NULL_HANDLE, NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, VK_SHADER_STAGE_FRAGMENT, VK_NULL_HANDLE, NULL },
	};

	VkPipelineInputAssemblyStateCreateInfo ia = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false,
	};

	VkPipelineViewportStateCreateInfo vp = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL,
		1,
	};

	VkPipelineRasterStateCreateInfo rs = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO, NULL,
		true, false, VK_FILL_MODE_SOLID, VK_CULL_MODE_NONE, VK_FRONT_FACE_CW,
	};

	VkPipelineMultisampleStateCreateInfo msaa = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL,
		1, false, 0.0f, 1,
	};

	VkPipelineDepthStencilStateCreateInfo ds = {
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL,
		false, false, VK_COMPARE_OP_ALWAYS, false, false,
		{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS },
		{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS },
	};

	VkPipelineColorBlendAttachmentState attState = {
		false,
		VK_BLEND_ONE, VK_BLEND_ZERO, VK_BLEND_OP_ADD,
		VK_BLEND_ONE, VK_BLEND_ZERO, VK_BLEND_OP_ADD,
		0xf,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL,
		false, false, VK_LOGIC_OP_NOOP,
		1, &attState,
	};

	VkGraphicsPipelineCreateInfo pipeInfo = {
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL,
		2, stages,
		NULL, // vertex input
		&ia,
		NULL, // tess
		&vp,
		&rs,
		&msaa,
		&ds,
		&cb,
		0, // flags
		Unwrap(m_CheckerboardPipeLayout),
		VK_NULL_HANDLE, // render pass
		0, // sub pass
		VK_NULL_HANDLE, // base pipeline handle
		0, // base pipeline index
	};

	stages[0].shader = Unwrap(shader[BLITVS]);
	stages[1].shader = Unwrap(shader[CHECKERBOARDFS]);

	vkr = vt->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, &m_CheckerboardPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VKMGR()->WrapResource(Unwrap(dev), m_CheckerboardPipeline);
	
	stages[0].shader = Unwrap(shader[BLITVS]);
	stages[1].shader = Unwrap(shader[TEXDISPLAYFS]);

	pipeInfo.layout = Unwrap(m_TexDisplayPipeLayout);

	vkr = vt->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, &m_TexDisplayPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VKMGR()->WrapResource(Unwrap(dev), m_TexDisplayPipeline);

	attState.blendEnable = true;
	attState.srcBlendColor = VK_BLEND_SRC_ALPHA;
	attState.destBlendColor = VK_BLEND_ONE_MINUS_SRC_ALPHA;

	vkr = vt->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, &m_TexDisplayBlendPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VKMGR()->WrapResource(Unwrap(dev), m_TexDisplayBlendPipeline);

	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	
	stages[0].shader = Unwrap(shader[TEXTVS]);
	stages[1].shader = Unwrap(shader[TEXTFS]);

	pipeInfo.layout = Unwrap(m_TextPipeLayout);

	vkr = vt->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, &m_TextPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VKMGR()->WrapResource(Unwrap(dev), m_TextPipeline);
	
	ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	attState.blendEnable = false;

	VkVertexInputBindingDescription vertexBind = {
		0, sizeof(Vec4f), VK_VERTEX_INPUT_STEP_RATE_VERTEX,
	};

	VkVertexInputAttributeDescription vertexAttr = {
		0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0,
	};

	VkPipelineVertexInputStateCreateInfo vi = {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL,
		1, &vertexBind,
		1, &vertexAttr,
	};

	pipeInfo.pVertexInputState = &vi;
	
	stages[0].shader = Unwrap(shader[GENERICVS]);
	stages[1].shader = Unwrap(shader[GENERICFS]);

	pipeInfo.layout = Unwrap(m_GenericPipeLayout);

	vkr = vt->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, &m_GenericPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VKMGR()->WrapResource(Unwrap(dev), m_GenericPipeline);

	for(size_t i=0; i < ARRAY_COUNT(module); i++)
	{
		vkr = vt->DestroyShader(Unwrap(dev), Unwrap(shader[i]));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(shader[i]);
		
		vkr = vt->DestroyShaderModule(Unwrap(dev), Unwrap(module[i]));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(module[i]);
	}

	{
		int width = FONT_TEX_WIDTH, height = FONT_TEX_HEIGHT;

		VkImageCreateInfo imInfo = {
			/*.sType =*/ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			/*.pNext =*/ NULL,
			/*.imageType =*/ VK_IMAGE_TYPE_2D,
			/*.format =*/ VK_FORMAT_R8_UNORM,
			/*.extent =*/ { width, height, 1 },
			/*.mipLevels =*/ 1,
			/*.arraySize =*/ 1,
			/*.samples =*/ 1,
			/*.tiling =*/ VK_IMAGE_TILING_LINEAR,
			/*.usage =*/ VK_IMAGE_USAGE_SAMPLED_BIT,
			/*.flags =*/ 0,
		};

		string font = GetEmbeddedResource(sourcecodepro_ttf);
		byte *ttfdata = (byte *)font.c_str();

		const int firstChar = int(' ') + 1;
		const int lastChar = 127;
		const int numChars = lastChar-firstChar;

		byte *buf = new byte[width*height];

		const float pixelHeight = 20.0f;

		stbtt_bakedchar chardata[numChars];
		int ret = stbtt_BakeFontBitmap(ttfdata, 0, pixelHeight, buf, width, height, firstChar, numChars, chardata);

		m_FontCharSize = pixelHeight;
		m_FontCharAspect = chardata->xadvance / pixelHeight;

		stbtt_fontinfo f = {0};
		stbtt_InitFont(&f, ttfdata, 0);

		int ascent = 0;
		stbtt_GetFontVMetrics(&f, &ascent, NULL, NULL);

		float maxheight = float(ascent)*stbtt_ScaleForPixelHeight(&f, pixelHeight);

		// create and fill image
		{
			vkr = vt->CreateImage(Unwrap(dev), &imInfo, &m_TextAtlas);
			RDCASSERT(vkr == VK_SUCCESS);
				
			VKMGR()->WrapResource(Unwrap(dev), m_TextAtlas);

			VkMemoryRequirements mrq;
			vkr = vt->GetImageMemoryRequirements(Unwrap(dev), Unwrap(m_TextAtlas), &mrq);
			RDCASSERT(vkr == VK_SUCCESS);

			VkImageSubresource subr = { VK_IMAGE_ASPECT_COLOR, 0, 0 };
			VkSubresourceLayout layout = { 0 };
			vt->GetImageSubresourceLayout(Unwrap(dev), Unwrap(m_TextAtlas), &subr, &layout);

			// allocate readback memory
			VkMemoryAllocInfo allocInfo = {
				VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO, NULL,
				mrq.size, driver->GetUploadMemoryIndex(mrq.memoryTypeBits),
			};

			vkr = vt->AllocMemory(Unwrap(dev), &allocInfo, &m_TextAtlasMem);
			RDCASSERT(vkr == VK_SUCCESS);
				
			VKMGR()->WrapResource(Unwrap(dev), m_TextAtlasMem);

			vkr = vt->BindImageMemory(Unwrap(dev), Unwrap(m_TextAtlas), Unwrap(m_TextAtlasMem), 0);
			RDCASSERT(vkr == VK_SUCCESS);
			
			VkImageViewCreateInfo viewInfo = {
				VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL,
				Unwrap(m_TextAtlas), VK_IMAGE_VIEW_TYPE_2D,
				imInfo.format,
				{ VK_CHANNEL_SWIZZLE_R, VK_CHANNEL_SWIZZLE_G, VK_CHANNEL_SWIZZLE_B, VK_CHANNEL_SWIZZLE_A },
				{ VK_IMAGE_ASPECT_COLOR, 0, 1, 0, 1, }
			};

			// VKTODOMED used for texture display, but eventually will have to be created on the fly
			// for whichever image we're viewing (and cached), not specifically created here.
			vkr = vt->CreateImageView(Unwrap(dev), &viewInfo, &m_TextAtlasView);
			RDCASSERT(vkr == VK_SUCCESS);
				
			VKMGR()->WrapResource(Unwrap(dev), m_TextAtlasView);

			// need to transition image into valid state, then upload
			VkCmdBuffer cmd = driver->GetCmd();
			VkQueue q = driver->GetQ();
			
			VkCmdBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO, NULL, VK_CMD_BUFFER_OPTIMIZE_SMALL_BATCH_BIT | VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT };

			vkr = vt->ResetCommandBuffer(Unwrap(cmd), 0);
			RDCASSERT(vkr == VK_SUCCESS);
			vkr = vt->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
			RDCASSERT(vkr == VK_SUCCESS);
			
			VkImageMemoryBarrier trans = {
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
				0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				0, 0, Unwrap(m_TextAtlas),
				{ VK_IMAGE_ASPECT_COLOR, 0, 1, 0, 1 } };

			void *barrier = (void *)&trans;

			vt->CmdPipelineBarrier(Unwrap(cmd), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, &barrier);

			vt->EndCommandBuffer(Unwrap(cmd));

			vt->QueueSubmit(Unwrap(q), 1, UnwrapPtr(cmd), VK_NULL_HANDLE);

			// VKTODOMED ideally all the commands from Bind to Flip would be recorded
			// into a single command buffer and we can just have several allocated
			// ring-buffer style
			vt->QueueWaitIdle(Unwrap(q));

			byte *pData = NULL;
			vkr = vt->MapMemory(Unwrap(dev), Unwrap(m_TextAtlasMem), 0, 0, 0, (void **)&pData);
			RDCASSERT(vkr == VK_SUCCESS);

			RDCASSERT(pData != NULL);

			for(int32_t row = 0; row < height; row++)
			{
				memcpy(pData, buf, width);
				pData += layout.rowPitch;
				buf += width;
			}

			vkr = vt->UnmapMemory(Unwrap(dev), Unwrap(m_TextAtlasMem));
			RDCASSERT(vkr == VK_SUCCESS);
		}

		m_TextGlyphUBO.Create(driver, dev, 4096);
		RDCCOMPILE_ASSERT(sizeof(Vec4f)*2*(numChars+1) < 4096, "font uniform size");

		Vec4f *glyphData = (Vec4f *)m_TextGlyphUBO.Map(vt, dev);

		for(int i=0; i < numChars; i++)
		{
			stbtt_bakedchar *b = chardata+i;

			float x = b->xoff;
			float y = b->yoff + maxheight;

			glyphData[(i+1)*2 + 0] = Vec4f(x/b->xadvance, y/pixelHeight, b->xadvance/float(b->x1 - b->x0), pixelHeight/float(b->y1 - b->y0));
			glyphData[(i+1)*2 + 1] = Vec4f(b->x0, b->y0, b->x1, b->y1);
		}

		m_TextGlyphUBO.Unmap(vt, dev);
	}

	VkDescriptorInfo desc[7];
	RDCEraseEl(desc);
	
	// checkerboard
	desc[0].bufferView = Unwrap(m_CheckerboardUBO.view);

	// tex display
	desc[1].bufferView = Unwrap(m_TexDisplayUBO.view);
	// image descriptor is updated right before rendering

	// text
	desc[2].bufferView = Unwrap(m_TextGeneralUBO.view);
	desc[3].bufferView = Unwrap(m_TextGlyphUBO.view);
	desc[4].bufferView = Unwrap(m_TextStringUBO.view);
	desc[5].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	desc[5].imageView = Unwrap(m_TextAtlasView);
	desc[5].sampler = Unwrap(m_LinearSampler);
	
	// generic
	desc[6].bufferView = Unwrap(m_GenericUBO.view);

	VkWriteDescriptorSet writeSet[] = {
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_CheckerboardDescSet), 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[0]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_TexDisplayDescSet), 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[1]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_TextDescSet), 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[2]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_TextDescSet), 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[3]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_TextDescSet), 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[4]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_TextDescSet), 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &desc[5]
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
			Unwrap(m_GenericDescSet), 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc[6]
		},
	};

	vkr = vt->UpdateDescriptorSets(Unwrap(dev), ARRAY_COUNT(writeSet), writeSet, 0, NULL);
	RDCASSERT(vkr == VK_SUCCESS);
}

VulkanDebugManager::~VulkanDebugManager()
{
	VkDevice dev = m_Device;
	const VkLayerDispatchTable *vt = ObjDisp(dev);

	VkResult vkr = VK_SUCCESS;

	if(m_DescriptorPool != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDescriptorPool(Unwrap(dev), Unwrap(m_DescriptorPool));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_DescriptorPool);
	}

	if(m_DynamicCBStateWhite != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDynamicColorBlendState(Unwrap(dev), Unwrap(m_DynamicCBStateWhite));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_DynamicCBStateWhite);
	}

	if(m_DynamicRSState != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDynamicRasterState(Unwrap(dev), Unwrap(m_DynamicRSState));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_DynamicRSState);
	}

	if(m_DynamicDSStateDisabled != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDynamicDepthStencilState(Unwrap(dev), Unwrap(m_DynamicDSStateDisabled));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_DynamicDSStateDisabled);
	}

	if(m_LinearSampler != VK_NULL_HANDLE)
	{
		vkr = vt->DestroySampler(Unwrap(dev), Unwrap(m_LinearSampler));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_LinearSampler);
	}

	if(m_PointSampler != VK_NULL_HANDLE)
	{
		vkr = vt->DestroySampler(Unwrap(dev), Unwrap(m_PointSampler));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_PointSampler);
	}

	if(m_CheckerboardDescSetLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDescriptorSetLayout(Unwrap(dev), Unwrap(m_CheckerboardDescSetLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_CheckerboardDescSetLayout);
	}

	if(m_CheckerboardPipeLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipelineLayout(Unwrap(dev), Unwrap(m_CheckerboardPipeLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_CheckerboardPipeLayout);
	}

	if(m_CheckerboardPipeline != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipeline(Unwrap(dev), Unwrap(m_CheckerboardPipeline));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_CheckerboardPipeline);
	}

	if(m_TexDisplayDescSetLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDescriptorSetLayout(Unwrap(dev), Unwrap(m_TexDisplayDescSetLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TexDisplayDescSetLayout);
	}

	if(m_TexDisplayPipeLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipelineLayout(Unwrap(dev), Unwrap(m_TexDisplayPipeLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TexDisplayPipeLayout);
	}

	if(m_TexDisplayPipeline != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipeline(Unwrap(dev), Unwrap(m_TexDisplayPipeline));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TexDisplayPipeline);
	}

	if(m_TexDisplayBlendPipeline != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipeline(Unwrap(dev), Unwrap(m_TexDisplayBlendPipeline));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TexDisplayBlendPipeline);
	}

	m_CheckerboardUBO.Destroy(vt, dev);
	m_TexDisplayUBO.Destroy(vt, dev);

	if(m_TextDescSetLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDescriptorSetLayout(Unwrap(dev), Unwrap(m_TextDescSetLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextDescSetLayout);
	}

	if(m_TextPipeLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipelineLayout(Unwrap(dev), Unwrap(m_TextPipeLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextPipeLayout);
	}

	if(m_TextPipeline != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipeline(Unwrap(dev), Unwrap(m_TextPipeline));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextPipeline);
	}

	m_TextGeneralUBO.Destroy(vt, dev);
	m_TextGlyphUBO.Destroy(vt, dev);
	m_TextStringUBO.Destroy(vt, dev);

	if(m_TextAtlasView != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyImageView(Unwrap(dev), Unwrap(m_TextAtlasView));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextAtlasView);
	}

	if(m_TextAtlas != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyImage(Unwrap(dev), Unwrap(m_TextAtlas));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextAtlas);
	}

	if(m_TextAtlasMem != VK_NULL_HANDLE)
	{
		vkr = vt->FreeMemory(Unwrap(dev), Unwrap(m_TextAtlasMem));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_TextAtlasMem);
	}
	
	if(m_GenericDescSetLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyDescriptorSetLayout(Unwrap(dev), Unwrap(m_GenericDescSetLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_GenericDescSetLayout);
	}

	if(m_GenericPipeLayout != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipelineLayout(Unwrap(dev), Unwrap(m_GenericPipeLayout));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_GenericPipeLayout);
	}

	if(m_GenericPipeline != VK_NULL_HANDLE)
	{
		vkr = vt->DestroyPipeline(Unwrap(dev), Unwrap(m_GenericPipeline));
		RDCASSERT(vkr == VK_SUCCESS);
		VKMGR()->ReleaseWrappedResource(m_GenericPipeline);
	}

	m_OutlineStripVBO.Destroy(vt, dev);
	m_GenericUBO.Destroy(vt, dev);
}

void VulkanDebugManager::RenderText(const TextPrintState &textstate, float x, float y, const char *textfmt, ...)
{
	static char tmpBuf[4096];

	va_list args;
	va_start(args, textfmt);
	StringFormat::vsnprintf( tmpBuf, 4095, textfmt, args );
	tmpBuf[4095] = '\0';
	va_end(args);

	RenderTextInternal(textstate, x, y, tmpBuf);
}

void VulkanDebugManager::RenderTextInternal(const TextPrintState &textstate, float x, float y, const char *text)
{
	const VkLayerDispatchTable *vt = ObjDisp(textstate.cmd);

	// VKTODOMED needs to be optimised to do all in one cmd buffer with
	// a start/stop pair of calls that map a UBO, then do each draw with
	// a push constant to tell it what the line should be.
	
	VkCmdBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO, NULL, VK_CMD_BUFFER_OPTIMIZE_SMALL_BATCH_BIT | VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT };

	VkResult vkr = VK_SUCCESS;

	fontuniforms *ubo = (fontuniforms *)m_TextGeneralUBO.Map(vt, m_Device);

	ubo->TextPosition.x = x;
	ubo->TextPosition.y = y;

	ubo->FontScreenAspect.x = 1.0f/float(textstate.w);
	ubo->FontScreenAspect.y = 1.0f/float(textstate.h);

	ubo->TextSize = m_FontCharSize;
	ubo->FontScreenAspect.x *= m_FontCharAspect;

	ubo->CharacterSize.x = 1.0f/float(FONT_TEX_WIDTH);
	ubo->CharacterSize.y = 1.0f/float(FONT_TEX_HEIGHT);

	m_TextGeneralUBO.Unmap(vt, m_Device);

	stringdata *stringData = (stringdata *)m_TextStringUBO.Map(vt, m_Device);

	for(size_t i=0; i < strlen(text); i++)
		stringData->str[i][0] = uint32_t(text[i] - ' ');

	m_TextStringUBO.Unmap(vt, m_Device);

	vkr = vt->ResetCommandBuffer(Unwrap(textstate.cmd), 0);
	RDCASSERT(vkr == VK_SUCCESS);
	vkr = vt->BeginCommandBuffer(Unwrap(textstate.cmd), &beginInfo);
	RDCASSERT(vkr == VK_SUCCESS);

	{
		VkClearValue clearval = {0};
		VkRenderPassBeginInfo rpbegin = {
			VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL,
			Unwrap(textstate.rp), Unwrap(textstate.fb),
			{ { 0, 0, }, { textstate.w, textstate.h} },
			1, &clearval,
		};
		vt->CmdBeginRenderPass(Unwrap(textstate.cmd), &rpbegin, VK_RENDER_PASS_CONTENTS_INLINE);

		vt->CmdBindPipeline(Unwrap(textstate.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, Unwrap(m_TextPipeline));
		vt->CmdBindDescriptorSets(Unwrap(textstate.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, Unwrap(m_TextPipeLayout), 0, 1, UnwrapPtr(m_TextDescSet), 0, NULL);

		vt->CmdBindDynamicViewportState(Unwrap(textstate.cmd), Unwrap(textstate.vp));
		vt->CmdBindDynamicRasterState(Unwrap(textstate.cmd), Unwrap(m_DynamicRSState));
		vt->CmdBindDynamicColorBlendState(Unwrap(textstate.cmd), Unwrap(m_DynamicCBStateWhite));
		vt->CmdBindDynamicDepthStencilState(Unwrap(textstate.cmd), Unwrap(m_DynamicDSStateDisabled));

		// VKTODOMED strip + instance ID doesn't seem to work atm? instance ID comes through 0
		// for now, do lists, but want to change back 
		vt->CmdDraw(Unwrap(textstate.cmd), 0, 6*(uint32_t)strlen(text), 0, 1);
		vt->CmdEndRenderPass(Unwrap(textstate.cmd));
	}

	vt->EndCommandBuffer(Unwrap(textstate.cmd));

	vt->QueueSubmit(Unwrap(textstate.q), 1, UnwrapPtr(textstate.cmd), VK_NULL_HANDLE);

	vt->QueueWaitIdle(Unwrap(textstate.q));
}
