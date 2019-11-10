/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DRW_render.h"

#include "ED_view3d.h"

#include "DNA_mesh_types.h"

#include "BKE_editmesh.h"

#include "draw_cache_impl.h"
#include "draw_manager_text.h"

#include "overlay_private.h"

#define OVERLAY_EDIT_TEXT \
  (V3D_OVERLAY_EDIT_EDGE_LEN | V3D_OVERLAY_EDIT_FACE_AREA | V3D_OVERLAY_EDIT_FACE_ANG | \
   V3D_OVERLAY_EDIT_EDGE_ANG | V3D_OVERLAY_EDIT_INDICES)

void OVERLAY_edit_mesh_init(OVERLAY_Data *vedata)
{
  OVERLAY_TextureList *txl = vedata->txl;
  OVERLAY_FramebufferList *fbl = vedata->fbl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  const DRWContextState *draw_ctx = DRW_context_state_get();

  /* TODO only alloc if needed. */
  DRW_texture_ensure_fullscreen_2d(&txl->temp_depth_tx, GPU_DEPTH_COMPONENT24, 0);
  DRW_texture_ensure_fullscreen_2d(&txl->edit_mesh_occlude_wire_tx, GPU_RGBA8, 0);

  GPU_framebuffer_ensure_config(&fbl->edit_mesh_occlude_wire_fb,
                                {GPU_ATTACHMENT_TEXTURE(txl->temp_depth_tx),
                                 GPU_ATTACHMENT_TEXTURE(txl->edit_mesh_occlude_wire_tx)});

  /* Create view with depth offset */
  pd->view_edit_faces = (DRWView *)DRW_view_default_get();
  pd->view_edit_faces_cage = DRW_view_create_with_zoffset(draw_ctx->rv3d, 0.5f);
  pd->view_edit_edges = DRW_view_create_with_zoffset(draw_ctx->rv3d, 1.0f);
  pd->view_edit_verts = DRW_view_create_with_zoffset(draw_ctx->rv3d, 1.5f);
}

void OVERLAY_edit_mesh_cache_init(OVERLAY_Data *vedata)
{
  OVERLAY_TextureList *txl = vedata->txl;
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  OVERLAY_ShadingData *shdata = &pd->shdata;
  DRWShadingGroup *grp = NULL;
  GPUShader *sh = NULL;
  DRWState state = 0;

  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();

  const DRWContextState *draw_ctx = DRW_context_state_get();
  ToolSettings *tsettings = draw_ctx->scene->toolsettings;
  View3D *v3d = draw_ctx->v3d;
  bool select_vert = pd->edit_mesh.select_vert = (tsettings->selectmode & SCE_SELECT_VERTEX) != 0;
  bool select_face = pd->edit_mesh.select_face = (tsettings->selectmode & SCE_SELECT_FACE) != 0;
  bool select_edge = pd->edit_mesh.select_edge = (tsettings->selectmode & SCE_SELECT_EDGE) != 0;

  pd->edit_mesh.do_zbufclip = XRAY_FLAG_ENABLED(v3d);

  bool do_occlude_wire = (v3d->overlay.edit_flag & V3D_OVERLAY_EDIT_OCCLUDE_WIRE) != 0;
  bool show_face_dots = (v3d->overlay.edit_flag & V3D_OVERLAY_EDIT_FACE_DOT) != 0 ||
                        pd->edit_mesh.do_zbufclip;

  pd->edit_mesh.ghost_ob = 0;
  pd->edit_mesh.edit_ob = 0;
  pd->edit_mesh.do_faces = true;
  pd->edit_mesh.do_edges = true;

  int *mask = shdata->data_mask;
  mask[0] = 0xFF; /* Face Flag */
  mask[1] = 0xFF; /* Edge Flag */

  const int flag = pd->edit_mesh.flag = v3d->overlay.edit_flag;

  SET_FLAG_FROM_TEST(mask[0], flag & V3D_OVERLAY_EDIT_FACES, VFLAG_FACE_SELECTED);
  SET_FLAG_FROM_TEST(mask[0], flag & V3D_OVERLAY_EDIT_FREESTYLE_FACE, VFLAG_FACE_FREESTYLE);
  SET_FLAG_FROM_TEST(mask[1], flag & V3D_OVERLAY_EDIT_FREESTYLE_EDGE, VFLAG_EDGE_FREESTYLE);
  SET_FLAG_FROM_TEST(mask[1], flag & V3D_OVERLAY_EDIT_SEAMS, VFLAG_EDGE_SEAM);
  SET_FLAG_FROM_TEST(mask[1], flag & V3D_OVERLAY_EDIT_SHARP, VFLAG_EDGE_SHARP);
  SET_FLAG_FROM_TEST(mask[2], flag & V3D_OVERLAY_EDIT_CREASES, 0xFF);
  SET_FLAG_FROM_TEST(mask[3], flag & V3D_OVERLAY_EDIT_BWEIGHTS, 0xFF);

  if ((flag & V3D_OVERLAY_EDIT_FACES) == 0) {
    pd->edit_mesh.do_faces = false;
    pd->edit_mesh.do_zbufclip = false;
  }
  if ((flag & V3D_OVERLAY_EDIT_EDGES) == 0) {
    if ((tsettings->selectmode & SCE_SELECT_EDGE) == 0) {
      if ((v3d->shading.type < OB_SOLID) || (v3d->shading.flag & V3D_SHADING_XRAY)) {
        /* Special case, when drawing wire, draw edges, see: T67637. */
      }
      else {
        pd->edit_mesh.do_edges = false;
      }
    }
  }

  float backwire_opacity = v3d->overlay.backwire_opacity;
  float size_normal = v3d->overlay.normals_length;
  float face_alpha = (do_occlude_wire || !pd->edit_mesh.do_faces) ? 0.0f : 1.0f;

  if (select_face && !pd->edit_mesh.do_faces && pd->edit_mesh.do_edges) {
    /* Force display of face centers in this case because that's
     * the only way to see if a face is selected. */
    show_face_dots = true;
  }

  {
    /* TODO(fclem) Shouldn't this be going into the paint overlay? */
    state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL;
    DRW_PASS_CREATE(psl->edit_mesh_weight_ps, state | pd->clipping_state);

    sh = OVERLAY_shader_paint_weight();
    pd->edit_mesh_weight_grp = grp = DRW_shgroup_create(sh, psl->edit_mesh_weight_ps);
    DRW_shgroup_uniform_float_copy(grp, "opacity", 1.0);
    DRW_shgroup_uniform_texture(grp, "colorramp", G_draw.weight_ramp);
    DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
  }
  /* Run Twice for in-front passes. */
  for (int i = 0; i < 2; i++) {
    /* Complementary Depth Pass */
    state = DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_CULL_BACK;
    DRW_PASS_CREATE(psl->edit_mesh_depth_ps[i], state | pd->clipping_state);

    sh = OVERLAY_shader_depth_only();
    pd->edit_mesh_depth_grp[i] = DRW_shgroup_create(sh, psl->edit_mesh_depth_ps[i]);
  }
  {
    /* Normals */
    state = DRW_STATE_WRITE_DEPTH | DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL;
    DRW_PASS_CREATE(psl->edit_mesh_normals_ps, state | pd->clipping_state);

    sh = OVERLAY_shader_edit_mesh_normal_face();
    pd->edit_mesh_fnormals_grp = grp = DRW_shgroup_create(sh, psl->edit_mesh_normals_ps);
    DRW_shgroup_uniform_float_copy(grp, "normalSize", size_normal);
    DRW_shgroup_uniform_vec4(grp, "color", G_draw.block.colorNormal, 1);

    sh = OVERLAY_shader_edit_mesh_normal_vert();
    pd->edit_mesh_vnormals_grp = grp = DRW_shgroup_create(sh, psl->edit_mesh_normals_ps);
    DRW_shgroup_uniform_float_copy(grp, "normalSize", size_normal);
    DRW_shgroup_uniform_vec4(grp, "color", G_draw.block.colorVNormal, 1);

    sh = OVERLAY_shader_edit_mesh_normal_loop();
    pd->edit_mesh_lnormals_grp = grp = DRW_shgroup_create(sh, psl->edit_mesh_normals_ps);
    DRW_shgroup_uniform_float_copy(grp, "normalSize", size_normal);
    DRW_shgroup_uniform_vec4(grp, "color", G_draw.block.colorLNormal, 1);
  }
  {
    /* Mesh Analysis Pass */
    state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA;
    DRW_PASS_CREATE(psl->edit_mesh_analysis_ps, state | pd->clipping_state);

    sh = OVERLAY_shader_edit_mesh_analysis();
    pd->edit_mesh_analysis_grp = grp = DRW_shgroup_create(sh, psl->edit_mesh_analysis_ps);
    DRW_shgroup_uniform_texture(grp, "weightTex", G_draw.weight_ramp);
  }
  /* Run Twice for in-front passes. */
  for (int i = 0; i < 2; i++) {
    GPUShader *edge_sh = OVERLAY_shader_edit_mesh_edge(!select_vert);
    GPUShader *face_sh = OVERLAY_shader_edit_mesh_face();
    const bool do_zbufclip = (i == 0 && pd->edit_mesh.do_zbufclip);
    DRWState state_common = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                            DRW_STATE_BLEND_ALPHA;
    /* Faces */
    /* Cage geom needs to be offsetted to avoid Z-fighting. */
    for (int j = 0; j < 2; j++) {
      DRWPass **edit_face_ps = (j == 0) ? &psl->edit_mesh_faces_ps[i] :
                                          &psl->edit_mesh_faces_cage_ps[i];
      DRWShadingGroup **shgrp = (j == 0) ? &pd->edit_mesh_faces_grp[i] :
                                           &pd->edit_mesh_faces_cage_grp[i];
      state = state_common;
      DRW_PASS_CREATE(*edit_face_ps, state | pd->clipping_state);

      grp = *shgrp = DRW_shgroup_create(face_sh, *edit_face_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_float_copy(grp, "faceAlphaMod", face_alpha);
      DRW_shgroup_uniform_ivec4(grp, "dataMask", mask, 1);
      DRW_shgroup_uniform_bool_copy(grp, "selectFaces", select_face);
    }

    if (do_zbufclip) {
      state_common |= DRW_STATE_WRITE_DEPTH;
      state_common &= ~DRW_STATE_BLEND_ALPHA;
    }

    /* Edges */
    /* Change first vertex convention to match blender loop structure. */
    state = state_common | DRW_STATE_FIRST_VERTEX_CONVENTION;
    DRW_PASS_CREATE(psl->edit_mesh_edges_ps[i], state | pd->clipping_state);

    grp = pd->edit_mesh_edges_grp[i] = DRW_shgroup_create(edge_sh, psl->edit_mesh_edges_ps[i]);
    DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
    DRW_shgroup_uniform_ivec4(grp, "dataMask", mask, 1);
    DRW_shgroup_uniform_bool_copy(grp, "selectEdges", pd->edit_mesh.do_edges || select_edge);

    /* Verts */
    state = state_common & ~DRW_STATE_BLEND_ALPHA;
    DRW_PASS_CREATE(psl->edit_mesh_verts_ps[i], state | pd->clipping_state);

    if (select_vert) {
      sh = OVERLAY_shader_edit_mesh_vert();
      grp = pd->edit_mesh_verts_grp[i] = DRW_shgroup_create(sh, psl->edit_mesh_verts_ps[i]);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);

      sh = OVERLAY_shader_edit_mesh_skin_root();
      grp = pd->edit_mesh_skin_roots_grp[i] = DRW_shgroup_create(sh, psl->edit_mesh_verts_ps[i]);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
    }
    /* Facedots */
    if (select_face && show_face_dots) {
      sh = OVERLAY_shader_edit_mesh_facedot();
      grp = pd->edit_mesh_facedots_grp[i] = DRW_shgroup_create(sh, psl->edit_mesh_verts_ps[i]);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_state_enable(grp, DRW_STATE_WRITE_DEPTH);
    }
    else {
      pd->edit_mesh_facedots_grp[i] = NULL;
    }
  }

  if (pd->edit_mesh.do_zbufclip) {
    state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA;
    DRW_PASS_CREATE(psl->edit_mesh_mix_occlude_ps, state);

    sh = OVERLAY_shader_edit_mesh_mix_occlude();
    grp = DRW_shgroup_create(sh, psl->edit_mesh_mix_occlude_ps);
    DRW_shgroup_uniform_float_copy(grp, "alpha", backwire_opacity);
    DRW_shgroup_uniform_texture_ref(grp, "wireColor", &txl->edit_mesh_occlude_wire_tx);
    DRW_shgroup_uniform_texture_ref(grp, "wireDepth", &txl->temp_depth_tx);
    DRW_shgroup_uniform_texture_ref(grp, "sceneDepth", &dtxl->depth);
    DRW_shgroup_call_procedural_triangles(grp, NULL, 1);
  }
}

static void overlay_edit_mesh_add_ob_to_pass(OVERLAY_PrivateData *pd, Object *ob, bool in_front)
{
  struct GPUBatch *geom_tris, *geom_verts, *geom_edges, *geom_fcenter, *skin_roots;
  DRWShadingGroup *vert_shgrp, *edge_shgrp, *fdot_shgrp, *face_shgrp, *skin_roots_shgrp;

  bool has_edit_mesh_cage = false;
  bool has_skin_roots = false;
  /* TODO: Should be its own function. */
  Mesh *me = (Mesh *)ob->data;
  BMEditMesh *embm = me->edit_mesh;
  if (embm) {
    has_edit_mesh_cage = embm->mesh_eval_cage && (embm->mesh_eval_cage != embm->mesh_eval_final);
    has_skin_roots = CustomData_get_offset(&embm->bm->vdata, CD_MVERT_SKIN) != -1;
  }

  vert_shgrp = pd->edit_mesh_verts_grp[in_front];
  edge_shgrp = pd->edit_mesh_edges_grp[in_front];
  fdot_shgrp = pd->edit_mesh_facedots_grp[in_front];
  face_shgrp = (has_edit_mesh_cage) ? pd->edit_mesh_faces_cage_grp[in_front] :
                                      pd->edit_mesh_faces_grp[in_front];
  skin_roots_shgrp = pd->edit_mesh_skin_roots_grp[in_front];

  geom_edges = DRW_mesh_batch_cache_get_edit_edges(ob->data);
  geom_tris = DRW_mesh_batch_cache_get_edit_triangles(ob->data);
  DRW_shgroup_call_no_cull(edge_shgrp, geom_edges, ob);
  DRW_shgroup_call_no_cull(face_shgrp, geom_tris, ob);

  if (pd->edit_mesh.select_vert) {
    geom_verts = DRW_mesh_batch_cache_get_edit_vertices(ob->data);
    DRW_shgroup_call_no_cull(vert_shgrp, geom_verts, ob);

    if (has_skin_roots) {
      DRWShadingGroup *grp = DRW_shgroup_create_sub(skin_roots_shgrp);
      /* We need to upload the matrix. But the ob can be temporary allocated so we cannot
       * use direct reference to ob->obmat. */
      DRW_shgroup_uniform_vec4_copy(grp, "editModelMat[0]", ob->obmat[0]);
      DRW_shgroup_uniform_vec4_copy(grp, "editModelMat[1]", ob->obmat[1]);
      DRW_shgroup_uniform_vec4_copy(grp, "editModelMat[2]", ob->obmat[2]);
      DRW_shgroup_uniform_vec4_copy(grp, "editModelMat[3]", ob->obmat[3]);

      skin_roots = DRW_mesh_batch_cache_get_edit_skin_roots(ob->data);
      /* NOTE(fclem) We cannot use ob here since it would offset the instance attribs with
       * base instance offset. */
      DRW_shgroup_call(grp, skin_roots, NULL);
    }
  }

  if (fdot_shgrp) {
    geom_fcenter = DRW_mesh_batch_cache_get_edit_facedots(ob->data);
    DRW_shgroup_call_no_cull(fdot_shgrp, geom_fcenter, ob);
  }
}

void OVERLAY_edit_mesh_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  struct GPUBatch *geom = NULL;

  bool do_in_front = (ob->dtx & OB_DRAWXRAY) != 0;
  bool do_occlude_wire = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_OCCLUDE_WIRE) != 0;
  bool do_show_weight = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_WEIGHT) != 0;
  bool do_show_mesh_analysis = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_STATVIS) != 0;
  bool fnormals_do = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_FACE_NORMALS) != 0;
  bool vnormals_do = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_VERT_NORMALS) != 0;
  bool lnormals_do = (pd->edit_mesh.flag & V3D_OVERLAY_EDIT_LOOP_NORMALS) != 0;

  if (do_show_weight) {
    geom = DRW_cache_mesh_surface_weights_get(ob);
    DRW_shgroup_call_no_cull(pd->edit_mesh_weight_grp, geom, ob);
  }
  else if (do_show_mesh_analysis && !pd->xray_enabled) {
    geom = DRW_cache_mesh_surface_mesh_analysis_get(ob);
    if (geom) {
      DRW_shgroup_call_no_cull(pd->edit_mesh_analysis_grp, geom, ob);
    }
  }

  if (do_occlude_wire || do_in_front) {
    geom = DRW_cache_mesh_surface_get(ob);
    DRW_shgroup_call_no_cull(pd->edit_mesh_depth_grp[do_in_front], geom, ob);
  }

  if (vnormals_do) {
    geom = DRW_mesh_batch_cache_get_edit_vnors(ob->data);
    DRW_shgroup_call_no_cull(pd->edit_mesh_vnormals_grp, geom, ob);
  }
  if (lnormals_do) {
    geom = DRW_mesh_batch_cache_get_edit_lnors(ob->data);
    DRW_shgroup_call_no_cull(pd->edit_mesh_lnormals_grp, geom, ob);
  }
  if (fnormals_do) {
    geom = DRW_mesh_batch_cache_get_edit_facedots(ob->data);
    DRW_shgroup_call_no_cull(pd->edit_mesh_fnormals_grp, geom, ob);
  }

  if (pd->edit_mesh.do_zbufclip) {
    overlay_edit_mesh_add_ob_to_pass(pd, ob, false);
  }
  else {
    overlay_edit_mesh_add_ob_to_pass(pd, ob, do_in_front);
  }

  pd->edit_mesh.ghost_ob += (ob->dtx & OB_DRAWXRAY) ? 1 : 0;
  pd->edit_mesh.edit_ob += 1;

  if (DRW_state_show_text() && (pd->edit_mesh.flag & OVERLAY_EDIT_TEXT)) {
    const DRWContextState *draw_ctx = DRW_context_state_get();
    DRW_text_edit_mesh_measure_stats(draw_ctx->ar, draw_ctx->v3d, ob, &draw_ctx->scene->unit);
  }
}

static void overlay_edit_mesh_draw_components(OVERLAY_PassList *psl,
                                              OVERLAY_PrivateData *pd,
                                              bool in_front)
{
  DRW_view_set_active(pd->view_edit_faces);
  DRW_draw_pass(psl->edit_mesh_faces_ps[in_front]);

  DRW_view_set_active(pd->view_edit_faces_cage);
  DRW_draw_pass(psl->edit_mesh_faces_cage_ps[in_front]);

  DRW_view_set_active(pd->view_edit_edges);
  DRW_draw_pass(psl->edit_mesh_edges_ps[in_front]);

  DRW_view_set_active(pd->view_edit_verts);
  DRW_draw_pass(psl->edit_mesh_verts_ps[in_front]);

  DRW_view_set_active(NULL);
}

void OVERLAY_edit_mesh_draw(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  OVERLAY_FramebufferList *fbl = vedata->fbl;
  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();

  DRW_draw_pass(psl->edit_mesh_weight_ps);
  DRW_draw_pass(psl->edit_mesh_analysis_ps);

  DRW_draw_pass(psl->edit_mesh_depth_ps[NOT_IN_FRONT]);

  if (pd->edit_mesh.do_zbufclip) {
    float clearcol[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    DRW_draw_pass(psl->edit_mesh_depth_ps[IN_FRONT]);

    /* render facefill */
    DRW_view_set_active(pd->view_edit_faces);
    DRW_draw_pass(psl->edit_mesh_faces_ps[NOT_IN_FRONT]);

    DRW_view_set_active(pd->view_edit_faces_cage);
    DRW_draw_pass(psl->edit_mesh_faces_cage_ps[NOT_IN_FRONT]);

    DRW_view_set_active(NULL);

    /* Render wires on a separate framebuffer */
    GPU_framebuffer_bind(fbl->edit_mesh_occlude_wire_fb);
    GPU_framebuffer_clear_color_depth(fbl->edit_mesh_occlude_wire_fb, clearcol, 1.0f);
    DRW_draw_pass(psl->edit_mesh_normals_ps);

    DRW_view_set_active(pd->view_edit_edges);
    DRW_draw_pass(psl->edit_mesh_edges_ps[NOT_IN_FRONT]);

    DRW_view_set_active(pd->view_edit_verts);
    DRW_draw_pass(psl->edit_mesh_verts_ps[NOT_IN_FRONT]);

    /* Combine with scene buffer */
    GPU_framebuffer_bind(dfbl->color_only_fb);
    DRW_draw_pass(psl->edit_mesh_mix_occlude_ps);
  }
  else {
    const DRWContextState *draw_ctx = DRW_context_state_get();
    View3D *v3d = draw_ctx->v3d;

    DRW_draw_pass(psl->edit_mesh_normals_ps);
    overlay_edit_mesh_draw_components(psl, pd, false);

    if (v3d->shading.type == OB_SOLID && pd->edit_mesh.ghost_ob == 1 &&
        pd->edit_mesh.edit_ob == 1) {
      /* In the case of single ghost object edit (common case for retopology):
       * we clear the depth buffer so that only the depth of the retopo mesh
       * is occluding the edit cage. */
      GPU_framebuffer_clear_depth(dfbl->default_fb, 1.0f);
    }

    DRW_draw_pass(psl->edit_mesh_depth_ps[1]);
    overlay_edit_mesh_draw_components(psl, pd, true);
  }
}