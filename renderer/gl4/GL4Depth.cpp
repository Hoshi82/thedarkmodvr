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
#include "../tr_local.h"
#include "GL4Backend.h"
#include "GLDebugGroup.h"
#include "OpenGL4Renderer.h"
#include "OcclusionSystem.h"

struct DepthFastDrawData {
	float modelMatrix[16];
};

struct DepthGenericDrawData {
	float modelMatrix[16];
	float textureMatrix[16];
	idPlane clipPlane;
	idVec4 color;
	idVec4 alphaTest;
};

const int SU_LOC_VIEWPROJ_MATRIX = 0;
const int SU_LOC_MODELVIEW_MATRIX = 1;
const int SU_LOC_CLIPPLANE = 2;
const int SU_LOC_TEXTURE_MATRIX = 3;
const int SU_LOC_COLOR = 4;
const int SU_LOC_ALPHATEST = 5;


void GL4_MultiDrawDepth( drawSurf_t **drawSurfs, int numDrawSurfs, bool staticVertex, bool staticIndex ) {
	if( numDrawSurfs == 0 ) {
		return;
	}

	GL_DEBUG_GROUP( MultiDrawDepth, DEPTH );

	DrawElementsIndirectCommand *commands = openGL4Renderer.ReserveCommandBuffer( numDrawSurfs );
	GLuint ssboSize = sizeof( DepthFastDrawData ) * numDrawSurfs;
	DepthFastDrawData *drawData = ( DepthFastDrawData* )openGL4Renderer.ReserveSSBO( ssboSize );

	int cmdIdx = 0;
	for( int i = 0; i < numDrawSurfs; ++i ) {
		if( r_useOcclusionCulling.GetBool() && occlusionSystem.WasEntityCulledLastFrame(drawSurfs[i]->space->entityIndex) ) {
			continue;
		}
		memcpy( drawData[cmdIdx].modelMatrix, drawSurfs[i]->space->modelMatrix, sizeof( drawData[cmdIdx].modelMatrix ) );
		const srfTriangles_t *tri = drawSurfs[i]->backendGeo;
		commands[cmdIdx].count = tri->numIndexes;
		commands[cmdIdx].instanceCount = 2;
		if( !tri->indexCache ) {
			common->Warning( "GL4_MultiDrawDepth: Missing indexCache" );
		}
		commands[cmdIdx].firstIndex = ( ( tri->indexCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( glIndex_t );
		commands[cmdIdx].baseVertex = ( ( tri->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( idDrawVert );
		commands[cmdIdx].baseInstance = cmdIdx;
		++cmdIdx;
	}

	openGL4Renderer.EnableVertexAttribs( { VA_POSITION, VA_DRAWID } );
	openGL4Renderer.BindVertexBuffer( staticVertex );
	vertexCache.IndexPosition( staticIndex ? 1 : 2 );
	openGL4Renderer.BindSSBO( 0, ssboSize );

	qglMultiDrawElementsIndirect( GL_TRIANGLES, GL_INDEX_TYPE, commands, cmdIdx, 0 );
	openGL4Renderer.MarkUsedSSBO( ssboSize );
}

void GL4_GenericDepth( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	GL_DEBUG_GROUP( GenericDepth, DEPTH );

	GL4Program depthShader = openGL4Renderer.GetShader( SHADER_DEPTH_GENERIC_STEREO );
	depthShader.Activate();
	depthShader.SetStereoViewProjectionMatrix( SU_LOC_VIEWPROJ_MATRIX );

	DepthGenericDrawData drawData;
	memcpy( drawData.textureMatrix, mat4_identity.ToFloatPtr(), sizeof( drawData.textureMatrix ) );

	// if we have no clip planes, set a noclip plane
	if( !backEnd.viewDef->numClipPlanes ) {
		drawData.clipPlane.ToVec4().Set( 0, 0, 0, 1 );
	}

	openGL4Renderer.EnableVertexAttribs( { VA_POSITION, VA_TEXCOORD } );

	// the first texture will be used for alpha tested surfaces
	GL_SelectTexture( 0 );
	openGL4Renderer.BindUBO( 0 );

	for( int i = 0; i < numDrawSurfs; ++i ) {
		drawSurf_t *surf = drawSurfs[i];
		if( r_useOcclusionCulling.GetBool() && occlusionSystem.WasEntityCulledLastFrame( surf->space->entityIndex ) ) {
			continue;
		}
		drawData.color.Set( 0, 0, 0, 1 );  // draw black by default

		const srfTriangles_t *tri = surf->backendGeo;
		const idMaterial *shader = surf->material;

		// change the scissor if needed
		if( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			GL4_SetCurrentScissor( surf->scissorRect );
		}

		// update the clip plane if needed
		if( backEnd.viewDef->numClipPlanes && surf->space != backEnd.currentSpace ) {
			R_GlobalPlaneToLocal( surf->space->modelMatrix, backEnd.viewDef->clipPlanes[0], drawData.clipPlane );
		}

		if( surf->space != backEnd.currentSpace ) {
			backEnd.currentSpace = surf->space;
			memcpy( drawData.modelMatrix, surf->space->modelMatrix, sizeof( drawData.modelMatrix ) );
		}

		// set polygon offset if necessary
		if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			qglEnable( GL_POLYGON_OFFSET_FILL );
			qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}

		// subviews will just down-modulate the color buffer by overbright
		if( shader->GetSort() == SS_SUBVIEW ) {
			GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS );
			drawData.color[0] = drawData.color[1] = drawData.color[2] = ( 1.0 / backEnd.overBright );
		}

		openGL4Renderer.BindVertexBuffer( vertexCache.CacheIsStatic( tri->ambientCache ) );
		GLuint baseVertex = ( ( tri->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( idDrawVert );

		bool drawSolid = false;

		if( shader->Coverage() == MC_OPAQUE ) {
			drawSolid = true;
		}

		// we may have multiple alpha tested stages
		if( shader->Coverage() == MC_PERFORATED ) {
			// if the only alpha tested stages are condition register omitted,
			// draw a normal opaque surface
			bool didDraw = false;

			// perforated surfaces may have multiple alpha tested stages
			const float *regs = surf->shaderRegisters;
			for( int stage = 0; stage < shader->GetNumStages(); stage++ ) {
				const shaderStage_t *pStage = shader->GetStage( stage );

				if( !pStage->hasAlphaTest ) {
					continue;
				}

				// check the stage enable condition
				if( regs[pStage->conditionRegister] == 0 ) {
					continue;
				}

				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;

				// set the alpha modulate
				drawData.color[3] = regs[pStage->color.registers[3]];

				// skip the entire stage if alpha would be black
				if( drawData.color[3] <= 0 ) {
					continue;
				}
				drawData.alphaTest.x = regs[pStage->alphaTestRegister];

				// bind the texture
				pStage->texture.image->Bind();

				// set privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset ) {
					qglEnable( GL_POLYGON_OFFSET_FILL );
					qglPolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
				}

				// set the texture matrix if needed
				if( pStage->texture.hasMatrix ) {
					float matrix[16];
					RB_GetShaderTextureMatrix( surf->shaderRegisters, &pStage->texture, matrix );
					memcpy( drawData.textureMatrix, matrix, sizeof( drawData.textureMatrix ) );
				}

				// draw it
				openGL4Renderer.UpdateUBO( &drawData, sizeof( DepthGenericDrawData ) );
				qglDrawElementsInstancedBaseVertex( GL_TRIANGLES, tri->numIndexes, GL_INDEX_TYPE, vertexCache.IndexPosition( tri->indexCache ), 2, baseVertex );

				// unset privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset && !surf->material->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
					qglDisable( GL_POLYGON_OFFSET_FILL );
				}

				if( pStage->texture.hasMatrix ) {
					memcpy( drawData.textureMatrix, mat4_identity.ToFloatPtr(), sizeof( drawData.textureMatrix ) );
				}
			}

			if( !didDraw ) {
				drawSolid = true;
			}
		}

		// draw the entire surface solid
		if( drawSolid ) {
			drawData.alphaTest.x = -1;

			// draw it
			openGL4Renderer.UpdateUBO( &drawData, sizeof( DepthGenericDrawData ) );
			qglDrawElementsInstancedBaseVertex( GL_TRIANGLES, tri->numIndexes, GL_INDEX_TYPE, vertexCache.IndexPosition( tri->indexCache ), 2, baseVertex );
		}

		// reset polygon offset
		if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) ) {
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		// reset blending
		if( shader->GetSort() == SS_SUBVIEW ) {
			GL_State( GLS_DEPTHFUNC_LESS );
		}
	}
}

void GL4_FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	// if we are just doing 2D rendering, no need to fill the depth buffer
	if( !backEnd.viewDef->viewEntitys ) {
		return;
	}

	GL_DEBUG_GROUP( FillDepthBuffer_GL4, DEPTH );

	// TODO: only needed while mixing with legacy rendering
	openGL4Renderer.PrepareVertexAttribs();

	GL_State( GLS_DEPTHFUNC_LESS /*| GLS_COLORMASK | GLS_ALPHAMASK*/ );  // may need to reset color to black on later views
	// Enable stencil test if we are going to be using it for shadows.
	// If we didn't do this, it would be legal behavior to get z fighting
	// from the ambient pass and the light passes.
	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_ALWAYS, 1, 255 );

	// divide list of draw surfs into the separate batches
	std::vector<drawSurf_t*> subViewSurfaces;
	std::vector<drawSurf_t*> staticVertexStaticIndex;
	std::vector<drawSurf_t*> staticVertexFrameIndex;
	std::vector<drawSurf_t*> frameVertexFrameIndex;
	std::vector<drawSurf_t*> remainingSurfaces;
	for( int i = 0; i < numDrawSurfs; ++i ) {
		drawSurf_t *surf = drawSurfs[i];
		const srfTriangles_t *tri = surf->backendGeo;
		const idMaterial *material = surf->material;

		if( !material->IsDrawn() || material->Coverage() == MC_TRANSLUCENT ) {
			// translucent surfaces don't put anything in the depth buffer
			continue;
		}

		// some deforms may disable themselves by setting numIndexes = 0
		if( !tri->numIndexes ) {
			continue;
		}

		if( !tri->ambientCache ) {
			common->Printf( "GL4_FillDepthBuffer: !tri->ambientCache\n" );
			continue;
		}

		if( !tri->indexCache ) {
			common->Printf( "GL4_FillDepthBuffer: !tri->indexCache\n" );
		}

		if( surf->material->GetSort() == SS_PORTAL_SKY && g_enablePortalSky.GetInteger() == 2 )
			continue;

		// get the expressions for conditionals / color / texcoords
		const float *regs = surf->shaderRegisters;
		// if all stages of a material have been conditioned off, don't do anything
		int stage;
		for( stage = 0; stage < material->GetNumStages(); stage++ ) {
			const shaderStage_t *pStage = material->GetStage( stage );
			// check the stage enable condition
			if( regs[pStage->conditionRegister] != 0 ) {
				break;
			}
		}
		if( stage == material->GetNumStages() ) {
			continue;
		}

		if( material->GetSort() == SS_SUBVIEW ) {
			// subview surfaces need to be rendered first in a generic pass (due to mirror plane clipping).
			subViewSurfaces.push_back( surf );
			continue;
		}

		if( material->Coverage() == MC_PERFORATED || material->TestMaterialFlag( MF_POLYGONOFFSET ) || surf->space->weaponDepthHack || surf->space->modelDepthHack != 0) {
			// these are objects that can't be handled by the fast depth pass. 
			// they will be postponed and rendered in a generic pass.
			remainingSurfaces.push_back( surf );
			continue;
		}

		if( !vertexCache.CacheIsStatic( tri->ambientCache ) ) {
			frameVertexFrameIndex.push_back( surf );
		}
		else {
			if( vertexCache.CacheIsStatic( tri->indexCache ) ) {
				staticVertexStaticIndex.push_back( surf );
			}
			else {
				staticVertexFrameIndex.push_back( surf );
			}
		}
	}

	if( !subViewSurfaces.empty() ) {
		GL4_GenericDepth( subViewSurfaces.data(), subViewSurfaces.size() );
	}


	// sort static objects by view distance to profit more from early Z
	std::sort( staticVertexStaticIndex.begin(), staticVertexStaticIndex.end(), []( const drawSurf_t* a, const drawSurf_t* b ) -> bool {
		float distA = ( a->space->entityDef->parms.origin - backEnd.viewDef->renderView.vieworg ).LengthSqr();
		float distB = ( b->space->entityDef->parms.origin - backEnd.viewDef->renderView.vieworg ).LengthSqr();
		return distA < distB;
	} );


	GL4Program depthShaderMD = openGL4Renderer.GetShader( SHADER_DEPTH_FAST_MD_STEREO );
	depthShaderMD.Activate();
	depthShaderMD.SetStereoViewProjectionMatrix( SU_LOC_VIEWPROJ_MATRIX );

	GL4_MultiDrawDepth( staticVertexStaticIndex.data(), staticVertexStaticIndex.size(), true, true );
	GL4_MultiDrawDepth( staticVertexFrameIndex.data(), staticVertexFrameIndex.size(), true, false );
	GL4_MultiDrawDepth( frameVertexFrameIndex.data(), frameVertexFrameIndex.size(), false, false );
	
	// draw all remaining surfaces with the general code path
	if( !remainingSurfaces.empty() ) {
		GL4_GenericDepth( &remainingSurfaces[0], remainingSurfaces.size() );
	}

	// TODO: should not be needed once full backend is ported
	openGL4Renderer.EnableVertexAttribs( {VA_POSITION} );

	GL_CheckErrors();

	GL4Program::Unset();
}