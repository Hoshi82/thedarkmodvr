/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/

#include "precompiled.h"
#include "tr_local.h"
#include "glsl.h"
#include "PersistentBufferObject.h"

static const int MAX_MULTIDRAW_OBJECTS = 1024;
static const int MULTIDRAW_STORAGE_FACTOR = 3;

struct DrawElementsIndirectCommand {
	uint count;
	uint instanceCount;
	uint firstIndex;
	uint baseVertex;
	uint baseInstance;
};

struct StencilDrawData {
	float  modelMatrix[16];
	idVec4 lightOrigin;
};

struct MultiDrawBuffers {
	GLuint drawIdBuffer;
	PersistentBufferObject<DrawElementsIndirectCommand> commandBuffer;
	PersistentBufferObject<StencilDrawData> stencilDrawDataBuffer;

	void Init() {
		if( drawIdBuffer != 0 )
			return;
		
		qglGenBuffersARB( 1, &drawIdBuffer );
		std::vector<uint32_t> drawIds( MAX_MULTIDRAW_OBJECTS );
		for( uint32_t i = 0; i < MAX_MULTIDRAW_OBJECTS; ++i )
			drawIds[i] = i;
		qglBindBufferARB( GL_ARRAY_BUFFER, drawIdBuffer );
		qglBufferDataARB( GL_ARRAY_BUFFER, drawIds.size() * sizeof( uint32_t ), drawIds.data(), GL_STATIC_DRAW );

		commandBuffer.Init( GL_DRAW_INDIRECT_BUFFER, MAX_MULTIDRAW_OBJECTS * MULTIDRAW_STORAGE_FACTOR );
		stencilDrawDataBuffer.Init( GL_SHADER_STORAGE_BUFFER, MAX_MULTIDRAW_OBJECTS * MULTIDRAW_STORAGE_FACTOR, glConfig.ssboOffsetAlignment );
	}

	void Shutdown() {
		commandBuffer.Destroy();
		stencilDrawDataBuffer.Destroy();
		qglDeleteBuffersARB( 1, &drawIdBuffer );
		drawIdBuffer = 0;
	}

	void BindDrawId(GLuint index) {
		qglBindBufferARB( GL_ARRAY_BUFFER, drawIdBuffer );
		qglVertexAttribIPointer( index, 1, GL_UNSIGNED_INT, sizeof( uint32_t ), 0 );
		qglVertexAttribDivisor( index, 1 );
		qglEnableVertexAttribArray( index );
	}

	MultiDrawBuffers() : drawIdBuffer(0) {}
} multiDrawBuffers;


void GL_MemoryBarrier() {
	qglMemoryBarrier( GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT );
}


void RB_MultiDrawStencil( const drawSurf_t* drawSurfs, bool external ) {
	static StencilDrawData* drawData = ( StencilDrawData* )Mem_Alloc16( sizeof( StencilDrawData ) * MAX_MULTIDRAW_OBJECTS );
	static DrawElementsIndirectCommand* commands = ( DrawElementsIndirectCommand* )Mem_Alloc16( sizeof( DrawElementsIndirectCommand )*MAX_MULTIDRAW_OBJECTS );

	int count = 0;
	for( const drawSurf_t *drawSurf = drawSurfs; drawSurf; drawSurf = drawSurf->nextOnLight ) {
		if( count == MAX_MULTIDRAW_OBJECTS )
			break; // TODO
		const srfTriangles_t *tri = drawSurf->backendGeo;
		if( vertexCache.CacheIsStatic( tri->shadowCache ) || vertexCache.CacheIsStatic( tri->indexCache ) )
			continue; // TODO

		bool isExt = false;
		int numIndexes = 0;
		if( !r_useExternalShadows.GetInteger() ) {
			numIndexes = tri->numIndexes;
		}
		else if( r_useExternalShadows.GetInteger() == 2 ) { // force to no caps for testing
			numIndexes = tri->numShadowIndexesNoCaps;
		}
		else if( !( drawSurf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
			// if we aren't inside the shadow projection, no caps are ever needed needed
			numIndexes = tri->numShadowIndexesNoCaps;
			isExt = true;
		}
		else if( !backEnd.vLight->viewInsideLight && !( drawSurf->backendGeo->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
			// if we are inside the shadow projection, but outside the light, and drawing
			// a non-infinite shadow, we can skip some caps
			if( backEnd.vLight->viewSeesShadowPlaneBits & drawSurf->backendGeo->shadowCapPlaneBits ) {
				// we can see through a rear cap, so we need to draw it, but we can skip the
				// caps on the actual surface
				numIndexes = tri->numShadowIndexesNoFrontCaps;
			}
			else {
				// we don't need to draw any caps
				numIndexes = tri->numShadowIndexesNoCaps;
			}
			isExt = true;
		}
		else {
			// must draw everything
			numIndexes = tri->numIndexes;
		}

		if( isExt != external )
			continue;

		memcpy( drawData[count].modelMatrix, drawSurf->space->modelMatrix, sizeof( drawData[0].modelMatrix ) );
		R_GlobalPointToLocal( drawSurf->space->modelMatrix, backEnd.vLight->globalLightOrigin, drawData[count].lightOrigin.ToVec3() );
		drawData[count].lightOrigin.w = 0.0f;

		commands[count].count = numIndexes;
		commands[count].instanceCount = 1;
		commands[count].firstIndex = ( ( tri->indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( glIndex_t );
		commands[count].baseVertex = ( ( tri->shadowCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( shadowCache_t );
		commands[count].baseInstance = count;
		++count;
	}
	if( count == 0 ) {
		return;
	}

	qglBufferDataARB( GL_SHADER_STORAGE_BUFFER, count * sizeof( StencilDrawData ), drawData, GL_STATIC_DRAW );

	qglMultiDrawElementsIndirect( GL_TRIANGLES, GL_INDEX_TYPE, commands, count, 0 );

}

void RB_StencilShadowPass_MultiDraw( const drawSurf_t* drawSurfs ) {
	static GLuint dataBuf = 0;
	if( dataBuf == 0 ) {
		qglGenBuffersARB( 1, &dataBuf );
	}
	multiDrawBuffers.Init();

	qglPushDebugGroup( GL_DEBUG_SOURCE_APPLICATION, 20001, -1, "StencilShadowPassMultiDraw" );

	stencilShadowShaderMultiDraw.Use();

	globalImages->BindNull();
	GL_State( GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS );
	if( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() ) {
		qglPolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
		qglEnable( GL_POLYGON_OFFSET_FILL );
	}

	qglStencilFunc( GL_ALWAYS, 1, 255 );
	GL_Cull( CT_TWO_SIDED );

	if( glConfig.depthBoundsTestAvailable && r_useDepthBoundsTest.GetBool() ) {
		qglEnable( GL_DEPTH_BOUNDS_TEST_EXT );
		qglDepthBoundsEXT( backEnd.vLight->scissorRect.zmin, backEnd.vLight->scissorRect.zmax );
	}

	GLint viewProjMatrixPos = qglGetUniformLocation( stencilShadowShaderMultiDraw.program, "viewProjectionMatrix" );
	float viewProjectionMatrix[16];
	myGlMultMatrix( backEnd.viewDef->worldSpace.modelViewMatrix, backEnd.viewDef->projectionMatrix, viewProjectionMatrix );
	qglUniformMatrix4fv( viewProjMatrixPos, 1, GL_FALSE, viewProjectionMatrix );

	qglBindBufferARB( GL_DRAW_INDIRECT_BUFFER, 0 );
	multiDrawBuffers.BindDrawId( 1 );
	qglVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( shadowCache_t ), vertexCache.VertexPosition( 2 ) );
	vertexCache.IndexPosition( 2 );
	qglBindBufferARB( GL_SHADER_STORAGE_BUFFER, dataBuf );
	qglBindBufferBase( GL_SHADER_STORAGE_BUFFER, 0, dataBuf );

	// render non-external shadows
	qglStencilOpSeparate( backEnd.viewDef->isMirror ? GL_FRONT : GL_BACK, GL_KEEP, tr.stencilDecr, GL_KEEP );
	qglStencilOpSeparate( backEnd.viewDef->isMirror ? GL_BACK : GL_FRONT, GL_KEEP, tr.stencilIncr, GL_KEEP );
	RB_MultiDrawStencil( drawSurfs, false );

	// render external shadows
	qglStencilOpSeparate( backEnd.viewDef->isMirror ? GL_FRONT : GL_BACK, GL_KEEP, GL_KEEP, tr.stencilIncr );
	qglStencilOpSeparate( backEnd.viewDef->isMirror ? GL_BACK : GL_FRONT, GL_KEEP, GL_KEEP, tr.stencilDecr );
	RB_MultiDrawStencil( drawSurfs, true );

	qglDisableVertexAttribArray( 1 );

	GL_CheckErrors();

	GL_Cull( CT_FRONT_SIDED );

	if( r_shadowPolygonFactor.GetFloat() || r_shadowPolygonOffset.GetFloat() )
		qglDisable( GL_POLYGON_OFFSET_FILL );

	if( glConfig.depthBoundsTestAvailable && r_useDepthBoundsTest.GetBool() )
		qglDisable( GL_DEPTH_BOUNDS_TEST_EXT );

	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	if( !r_softShadowsQuality.GetBool() || backEnd.viewDef->renderView.viewID < TR_SCREEN_VIEW_ID )
		qglStencilFunc( GL_GEQUAL, 128, 255 );

	qglPopDebugGroup();
}