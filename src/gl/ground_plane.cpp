// Copyright 2017-2019, Nicholas Sharp and the Polyscope contributors. http://polyscope.run.
#include "polyscope/gl/ground_plane.h"

#include "polyscope/gl/gl_utils.h"
#include "polyscope/gl/materials/materials.h"
#include "polyscope/gl/shaders/ground_plane_shaders.h"
#include "polyscope/polyscope.h"

#include "imgui.h"
#include "stb_image.h"

namespace polyscope {

namespace gl {

// Storage for global options
bool groundPlaneEnabled = true;
float groundPlaneHeightFactor = 0.0;

// The concrete texture, stored in a separate binary unit
extern const std::array<unsigned char, 758403> bindata_concrete_seamless;

// Global variables and helpers for ground plane
namespace {

bool groundPlanePrepared = false;
gl::GLProgram* groundPlaneProgram = nullptr;
gl::GLTexturebuffer* mirroredSceneColorTexture = nullptr;
gl::GLFramebuffer* mirroredSceneFramebuffer = nullptr;

// cache which direction the ground plane faces
view::UpDir groundPlaneViewCached = view::UpDir::XUp; // not actually valid, must populate first time

void populateGroundPlaneGeometry() {

  int iP = 0;
  switch (view::upDir) {
  case view::UpDir::XUp:
    iP = 0;
    break;
  case view::UpDir::YUp:
    iP = 1;
    break;
  case view::UpDir::ZUp:
    iP = 2;
    break;
  }


  // Geometry of the ground plane, using triangles with vertices at infinity
  // clang-format off
  glm::vec4 cVert{0., 0., 0., 1.};
  glm::vec4 v1{0., 0., 0., 0.}; v1[(iP+2)%3] =  1.;
  glm::vec4 v2{0., 0., 0., 0.}; v2[(iP+1)%3] =  1.;
  glm::vec4 v3{0., 0., 0., 0.}; v3[(iP+2)%3] = -1.;
  glm::vec4 v4{0., 0., 0., 0.}; v4[(iP+1)%3] = -1.;
  
  std::vector<glm::vec4> positions = {
    cVert, v2, v1,
    cVert, v3, v2,
    cVert, v4, v3,
    cVert, v1, v4
  };
  // clang-format on

  groundPlaneProgram->setAttribute("a_position", positions);
  groundPlaneViewCached = view::upDir;
}

void prepareGroundPlane() {

  // The program that draws the ground plane
  groundPlaneProgram = new gl::GLProgram(&GROUND_PLANE_VERT_SHADER, &GROUND_PLANE_FRAG_SHADER, gl::DrawMode::Triangles);

  populateGroundPlaneGeometry();

  { // Load the ground texture
    int w, h, comp;
    unsigned char* image = nullptr;
    image = stbi_load("concrete_seamless.jpg", &w, &h, &comp, STBI_rgb);
    image = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(&gl::bindata_concrete_seamless[0]),
                                  gl::bindata_concrete_seamless.size(), &w, &h, &comp, STBI_rgb);
    if (image == nullptr) throw std::logic_error("Failed to load material image");
    groundPlaneProgram->setTexture2D("t_ground", image, w, h, false, false, true);
  }


  { // Mirored scene buffer
    using namespace gl;
    mirroredSceneColorTexture = new GLTexturebuffer(GL_RGBA, view::bufferWidth, view::bufferHeight);
    GLRenderbuffer* mirroredSceneDepthBuffer =
        new GLRenderbuffer(RenderbufferType::Depth, view::bufferWidth, view::bufferHeight);

    mirroredSceneFramebuffer = new GLFramebuffer();
    mirroredSceneFramebuffer->bindToColorTexturebuffer(mirroredSceneColorTexture);
    mirroredSceneFramebuffer->bindToDepthRenderbuffer(mirroredSceneDepthBuffer);

    groundPlaneProgram->setTextureFromBuffer("t_mirrorImage", mirroredSceneColorTexture);
  }


  groundPlanePrepared = true;
}
}; // namespace


void drawGroundPlane() {

  if (!groundPlaneEnabled) return;

  // don't draw ground in planar mode
  if (view::style == view::NavigateStyle::Planar) return;

  if (!groundPlanePrepared) {
    prepareGroundPlane();
  }
  if (view::upDir != groundPlaneViewCached) {
    populateGroundPlaneGeometry();
  }

  // Get logical "up" direction to which the ground plane is oriented
  int iP = 0;
  switch (view::upDir) {
  case view::UpDir::XUp:
    iP = 0;
    break;
  case view::UpDir::YUp:
    iP = 1;
    break;
  case view::UpDir::ZUp:
    iP = 2;
    break;
  }
  glm::vec3 baseUp{0., 0., 0.};
  glm::vec3 baseForward{0., 0., 0.};
  glm::vec3 baseRight{0., 0., 0.};
  baseUp[iP] = 1.;
  baseForward[(iP + 1) % 3] = 1.;
  baseRight[(iP + 2) % 3] = 1.;

  // Location for ground plane
  double bboxBottom = std::get<0>(state::boundingBox)[iP];
  double bboxHeight = std::get<1>(state::boundingBox)[iP] - std::get<0>(state::boundingBox)[iP];
  double heightEPS = state::lengthScale * 1e-4;
  double groundHeight = bboxBottom - groundPlaneHeightFactor * bboxHeight - heightEPS;

  // Implement the mirror effect
  {

    // Get the current framebuffer, so we can restore it after
    GLint drawFboId = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFboId);

    // Render to a texture so we can sample from it on the ground
    mirroredSceneFramebuffer->resizeBuffers(view::bufferWidth, view::bufferHeight);
    mirroredSceneFramebuffer->setViewport(0, 0, view::bufferWidth, view::bufferHeight);
    mirroredSceneFramebuffer->bindForRendering();
    mirroredSceneFramebuffer->clearColor = {view::bgColor[0], view::bgColor[1], view::bgColor[2]};
    mirroredSceneFramebuffer->clear();

    // Push a reflected view matrix
    glm::mat4 origViewMat = view::viewMat;

    glm::vec3 mirrorN = baseUp;
    glm::mat3 mirrorMat3 = glm::mat3(1.0) - 2.0f * glm::outerProduct(mirrorN, mirrorN);
    glm::vec3 tVec{0., 0., 0.};
    tVec[iP] = -groundHeight;
    glm::mat4 mirrorMat =
        glm::translate(glm::mat4(1.0), -tVec) * glm::mat4(mirrorMat3) * glm::translate(glm::mat4(1.0), tVec);
    view::viewMat = view::viewMat * mirrorMat;

    // Draw everything
    drawStructures();

    // Restore original view matrix
    view::viewMat = origViewMat;

    // Rebind to the previous framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, drawFboId);
  }

  // Set uniforms
  glm::mat4 viewMat = view::getCameraViewMatrix();
  groundPlaneProgram->setUniform("u_viewMatrix", glm::value_ptr(viewMat));

  glm::mat4 projMat = view::getCameraPerspectiveMatrix();
  groundPlaneProgram->setUniform("u_projMatrix", glm::value_ptr(projMat));

  glm::vec2 viewportDim{view::bufferWidth, view::bufferHeight};
  groundPlaneProgram->setUniform("u_viewportDim", viewportDim);

  groundPlaneProgram->setUniform("u_center", state::center);
  groundPlaneProgram->setUniform("u_basisX", baseForward);
  groundPlaneProgram->setUniform("u_basisY", baseRight);
  groundPlaneProgram->setUniform("u_basisZ", baseUp);

  float camHeight = view::getCameraWorldPosition()[iP];
  groundPlaneProgram->setUniform("u_cameraHeight", camHeight);

  groundPlaneProgram->setUniform("u_lengthScale", state::lengthScale);
  groundPlaneProgram->setUniform("u_groundHeight", groundHeight);

  glEnable(GL_BLEND);
  glDepthFunc(GL_LESS); // return to normal
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  groundPlaneProgram->draw();
}

void buildGroundPlaneGui() {

  ImGui::SetNextTreeNodeOpen(false, ImGuiCond_FirstUseEver);
  if (ImGui::TreeNode("ground plane")) {

    ImGui::Checkbox("Enabled", &groundPlaneEnabled);

    ImGui::SliderFloat("Height", &groundPlaneHeightFactor, 0.0, 1.0);


    ImGui::TreePop();
  }
}

void deleteGroundPlaneResources() {

  if (!groundPlanePrepared) {
    delete mirroredSceneColorTexture;
    delete mirroredSceneFramebuffer->getDepthRenderBuffer();
    delete mirroredSceneFramebuffer;
    safeDelete(groundPlaneProgram);
  }
}
} // namespace gl
} // namespace polyscope
