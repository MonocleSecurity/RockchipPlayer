#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#define GLFW_EXPOSE_NATIVE_EGL

#include <arpa/inet.h>
#include <atomic>
#include <boost/optional.hpp>
#include <boost/scope_exit.hpp>
#include <cstring>
#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <fstream>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <linux/dma-heap.h>
#include <map>
#include <memory>
#include <optional>
#include <rga/im2d.hpp>
#include <rga/im2d_buffer.h>
#include <rga/rga.h>
#include <rga/RgaUtils.h>
#include <rga/RockchipRga.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>
#include <rockchip/vpu_api.h>
#include <rockchip/mpp_err.h>
#include <rockchip/mpp_task.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi_cmd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#define GL_CHECK(stmt) stmt; GLCheckError(#stmt, __FILE__, __LINE__);

struct FRAME_BUFFER
{
  FRAME_BUFFER(const GLuint frame, const GLuint texture, const GLsizei width, const GLsizei height)
    : frame_(frame)
    , texture_(texture)
    , width_(width)
    , height_(height)
  {
  }

  ~FRAME_BUFFER()
  {
    glDeleteFramebuffers(1, &frame_);
    glDeleteTextures(1, &texture_);
  }

  GLuint frame_;
  GLuint texture_;
  GLsizei width_;
  GLsizei height_;

};

struct EGL_FRAME
{
  EGL_FRAME(const EGLImageKHR image, const MppFrameColorSpace colour_space, const MppFrameColorRange colour_range, const RK_U32 width, const RK_U32 height)
    : image_(image)
    , colour_space_(colour_space)
    , colour_range_(colour_range)
    , width_(width)
    , height_(height)
  {
  }

  EGLImageKHR image_;
  MppFrameColorSpace colour_space_;
  MppFrameColorRange colour_range_;
  RK_U32 width_;
  RK_U32 height_;

};

const uint8_t H264_START_SEQUENCE[] = { 0, 0, 0, 1 };
const std::string GLSL_VERSION_STRING("#version 320 es");
const std::string vertex_shader = GLSL_VERSION_STRING + "\n"
                                  "#undef lowp\n#undef mediump\n#undef highp\nprecision mediump float;\n"
                                  "in vec2 position;\n"
                                  "in vec2 texcoord;\n"
                                  "out vec2 outtexcoord;\n"
                                  "void main()\n"
                                  "{\n"
                                  "  gl_Position = vec4(position, 0.0, 1.0);\n"
                                  "  outtexcoord = vec2(texcoord.x, texcoord.y);\n"
                                  "}";
const std::string oes_fragment_shader = GLSL_VERSION_STRING + "\n"
                                        "#extension GL_OES_EGL_image_external : require\n"
                                        "#undef lowp\n#undef mediump\n#undef highp\nprecision mediump float;\n"
                                        "in vec2 outtexcoord;\n"
                                        "out vec4 colour;\n"
                                        "uniform samplerExternalOES tex;\n"
                                        "void main()\n"
                                        "{\n"
                                        "  colour = texture(tex, outtexcoord.st);\n"
                                        "}";
const std::string fragment_shader = GLSL_VERSION_STRING + "\n"
                                    "#undef lowp\n#undef mediump\n#undef highp\nprecision mediump float;\n"
                                    "in vec2 outtexcoord;\n"
                                    "out vec4 colour;\n"
                                    "uniform sampler2D tex;\n"
                                    "void main()\n"
                                    "{\n"
                                    "  colour = texture(tex, outtexcoord.st);\n"
                                    "}";
const std::vector<std::pair<MppFrameColorSpace, std::string>> MPP_COLOUR_SPACES =
{
  std::make_pair(MPP_FRAME_SPC_RGB, "MPP_FRAME_SPC_RGB"),
  std::make_pair(MPP_FRAME_SPC_BT709, "MPP_FRAME_SPC_BT709"),
  std::make_pair(MPP_FRAME_SPC_UNSPECIFIED, "MPP_FRAME_SPC_UNSPECIFIED"),
  std::make_pair(MPP_FRAME_SPC_FCC, "MPP_FRAME_SPC_FCC"),
  std::make_pair(MPP_FRAME_SPC_BT470BG, "MPP_FRAME_SPC_BT470BG"),
  std::make_pair(MPP_FRAME_SPC_SMPTE170M, "MPP_FRAME_SPC_SMPTE170M"),
  std::make_pair(MPP_FRAME_SPC_SMPTE240M, "MPP_FRAME_SPC_SMPTE240M"),
  std::make_pair(MPP_FRAME_SPC_YCOCG, "MPP_FRAME_SPC_YCOCG"),
  std::make_pair(MPP_FRAME_SPC_BT2020_NCL, "MPP_FRAME_SPC_BT2020_NCL"),
  std::make_pair(MPP_FRAME_SPC_BT2020_CL, "MPP_FRAME_SPC_BT2020_CL"),
  std::make_pair(MPP_FRAME_SPC_SMPTE2085, "MPP_FRAME_SPC_SMPTE2085"),
  std::make_pair(MPP_FRAME_SPC_CHROMA_DERIVED_NCL, "MPP_FRAME_SPC_CHROMA_DERIVED_NCL"),
  std::make_pair(MPP_FRAME_SPC_CHROMA_DERIVED_CL, "MPP_FRAME_SPC_CHROMA_DERIVED_CL"),
  std::make_pair(MPP_FRAME_SPC_ICTCP, "MPP_FRAME_SPC_ICTCP")
};
const std::vector<std::pair<MppFrameColorRange, std::string>> MPP_COLOUR_RANGES =
{
  std::make_pair(MPP_FRAME_RANGE_UNSPECIFIED, "MPP_FRAME_RANGE_UNSPECIFIED"),
  std::make_pair(MPP_FRAME_RANGE_MPEG, "MPP_FRAME_RANGE_MPEG"),
  std::make_pair(MPP_FRAME_RANGE_JPEG, "MPP_FRAME_RANGE_JPEG")
};
const std::vector<std::pair<MppFrameColorPrimaries, std::string>> MPP_COLOUR_PRIMARIES =
{
  std::make_pair(MPP_FRAME_PRI_BT709, "MPP_FRAME_PRI_BT709"),
  std::make_pair(MPP_FRAME_PRI_UNSPECIFIED, "MPP_FRAME_PRI_UNSPECIFIED"),
  std::make_pair(MPP_FRAME_PRI_BT470M, "MPP_FRAME_PRI_BT470M"),
  std::make_pair(MPP_FRAME_PRI_BT470BG, "MPP_FRAME_PRI_BT470BG"),
  std::make_pair(MPP_FRAME_PRI_SMPTE170M, "MPP_FRAME_PRI_SMPTE170M"),
  std::make_pair(MPP_FRAME_PRI_SMPTE240M, "MPP_FRAME_PRI_SMPTE240M"),
  std::make_pair(MPP_FRAME_PRI_FILM, "MPP_FRAME_PRI_FILM"),
  std::make_pair(MPP_FRAME_PRI_BT2020, "MPP_FRAME_PRI_BT2020"),
  std::make_pair(MPP_FRAME_PRI_SMPTEST428_1, "MPP_FRAME_PRI_SMPTEST428_1"),
  std::make_pair(MPP_FRAME_PRI_SMPTE431, "MPP_FRAME_PRI_SMPTE431"),
  std::make_pair(MPP_FRAME_PRI_SMPTE432, "MPP_FRAME_PRI_SMPTE432"),
  std::make_pair(MPP_FRAME_PRI_JEDEC_P22, "MPP_FRAME_PRI_JEDEC_P22")
};
const std::vector<std::pair<int, std::string>> EGL_COLOUR_SPACES =
{
  std::make_pair(EGL_ITU_REC601_EXT, "EGL_ITU_REC601_EXT"),
  std::make_pair(EGL_ITU_REC709_EXT, "EGL_ITU_REC709_EXT"),
  std::make_pair(EGL_ITU_REC2020_EXT, "EGL_ITU_REC2020_EXT")
};
const std::vector<std::pair<int, std::string>> EGL_COLOUR_RANGES =
{
  std::make_pair(EGL_YUV_FULL_RANGE_EXT, "EGL_YUV_FULL_RANGE_EXT"),
  std::make_pair(EGL_YUV_NARROW_RANGE_EXT, "EGL_YUV_NARROW_RANGE_EXT")
};
std::atomic<bool> running = true;

void sig(const int signum)
{
  running = false;
}

const char* GetColourSpaceText(const boost::optional<MppFrameColorSpace>& mpp_colour_space)
{
  if (!mpp_colour_space.is_initialized())
  {
    return "Unknown";
  }
  std::vector<std::pair<MppFrameColorSpace, std::string>>::const_iterator i = std::find_if(MPP_COLOUR_SPACES.cbegin(), MPP_COLOUR_SPACES.cend(), [&mpp_colour_space](const std::pair<MppFrameColorSpace, std::string>& i){ return (i.first == *mpp_colour_space); });
  if (i == MPP_COLOUR_SPACES.cend())
  {
    return "Unknown";
  }
  return i->second.c_str();
}

const char* GetColourRangeText(const boost::optional<MppFrameColorRange>& mpp_colour_range)
{
  if (!mpp_colour_range.is_initialized())
  {
    return "Unknown";
  }
  std::vector<std::pair<MppFrameColorRange, std::string>>::const_iterator i = std::find_if(MPP_COLOUR_RANGES.cbegin(), MPP_COLOUR_RANGES.cend(), [&mpp_colour_range](const std::pair<MppFrameColorRange, std::string>& i){ return (i.first == *mpp_colour_range); });
  if (i == MPP_COLOUR_RANGES.cend())
  {
    return "Unknown";
  }
  return i->second.c_str();
}

const char* GetColourPrimariesText(const boost::optional<MppFrameColorPrimaries>& mpp_colour_primaries)
{
  if (!mpp_colour_primaries.is_initialized())
  {
    return "Unknown";
  }
  std::vector<std::pair<MppFrameColorPrimaries, std::string>>::const_iterator i = std::find_if(MPP_COLOUR_PRIMARIES.cbegin(), MPP_COLOUR_PRIMARIES.cend(), [&mpp_colour_primaries](const std::pair<MppFrameColorPrimaries, std::string>& i){ return (i.first == *mpp_colour_primaries); });
  if (i == MPP_COLOUR_PRIMARIES.cend())
  {
    return "Unknown";
  }
  return i->second.c_str();
}

void GLCheckError(const char* stmt, const char* filename, const int line)
{
  int count = 0; // Just in case we recursively create errors we need to stop
  while (true)
  {
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR)
    {
      return;
    }
    if (++count >= 20)
    {
      std::terminate();
    }
  }
}

int CopyBuffer(const uint8_t* ptr, const size_t size, MppPacket& packet, std::unique_ptr<char[]>& packet_buffer, size_t& packet_buffer_size)
{
  const size_t nal_size = size + sizeof(H264_START_SEQUENCE);
  if ((packet_buffer == nullptr) || (nal_size > packet_buffer_size))
  {
    packet_buffer = std::make_unique<char[]>(nal_size);
    packet_buffer_size = nal_size;
    if (mpp_packet_deinit(&packet) != MPP_OK)
    {
      std::cout << "Failed to deinit packet" << std::endl;
      return -1;
    }
    if (mpp_packet_init(&packet, packet_buffer.get(), packet_buffer_size) != MPP_OK)
    {
      std::cout << "Failed to init packet" << std::endl;
      return -2;
    }
  }
  memcpy(packet_buffer.get(), H264_START_SEQUENCE, sizeof(H264_START_SEQUENCE));
  memcpy(packet_buffer.get() + 4, ptr, size);
  mpp_packet_write(packet, 0, packet_buffer.get(), nal_size);
  mpp_packet_set_pos(packet, packet_buffer.get());
  mpp_packet_set_length(packet, nal_size);
  return 0;
}

int SendFrame(MppApi* api, MppCtx context, const uint8_t* ptr, const size_t size, MppPacket& packet, std::unique_ptr<char[]>& packet_buffer, size_t& packet_buffer_size)
{
  if (CopyBuffer(ptr, size, packet, packet_buffer, packet_buffer_size))
  {
    std::cout << "Failed to copy buffer" << std::endl;
    return -1;
  }
  const int ret = api->decode_put_packet(context, packet);
  if (ret != MPP_OK)
  {
    std::cout << "Failed to place packet: " << ret << std::endl;
    return -2;
  }
  return 0;
}

int CreateShader(GLuint program, GLenum type, const char* source, int size)
{
  const GLuint shader = glCreateShader(type);
  if (shader == 0)
  {
    std::cout << "Failed to create shader program" << std::endl;
    return -1;
  }
  GL_CHECK(glShaderSource(shader, 1, &source, &size));
  GL_CHECK(glCompileShader(shader));
  int result = GL_FALSE;
  GL_CHECK(glGetShaderiv(shader, GL_COMPILE_STATUS, &result));
  if (result != GL_TRUE)
  {
    char error_log[1024];
    std::memset(&error_log[0], 0, sizeof(error_log));
    GL_CHECK(glGetShaderInfoLog(shader, sizeof(error_log), nullptr, &error_log[0]));
    std::cout << "Failed to compile shader " << std::endl << source << std::endl << std::endl << error_log << std::endl;
    GL_CHECK(glDeleteShader(shader));
    return -2;
  }
  GL_CHECK(glAttachShader(program, shader));
  GL_CHECK(glDeleteShader(shader));
  return 0;
}

void DestroyEGLFrames(PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_khr, std::map<MppBuffer, EGL_FRAME>& egl_images)
{
  for (const std::pair<MppBuffer, EGL_FRAME>& egl_image : egl_images)
  {
    if (egl_destroy_image_khr(glfwGetEGLDisplay(), egl_image.second.image_) != EGL_TRUE)
    {
      std::cout << "Failed to destroy EGL image" << std::endl;
    }
  }
  egl_images.clear();
}

int main(int argc, char** argv)
{
  // Args
  if (argc < 2)
  {
    std::cout << "./RockchipPlayer test.mp4" << std::endl;
    return -1;
  }
  // Signals
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = sig;
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGINT, &sa, nullptr))
  {
    std::cout << "Failed to register SIGINT" << std::endl;
    return -2;
  }
  if (sigaction(SIGTERM, &sa, nullptr))
  {
    std::cout << "Failed to register SIGTERM" << std::endl;
    return -3;
  }
  // Open the file
  std::cout << "Opening the file: " << argv[1] << std::endl;
  AVFormatContext* format_context = nullptr;
  if (avformat_open_input(&format_context, argv[1], nullptr, nullptr) != 0)
  {
    std::cout << "Failed to open avformat file: " << argv[1] << std::endl;
    return -4;
  }
  BOOST_SCOPE_EXIT(format_context)
  {
    avformat_close_input(&format_context);
  }
  BOOST_SCOPE_EXIT_END
  if (avformat_find_stream_info(format_context, nullptr) < 0)
  {
    std::cout << "Failed to find stream info: " << argv[1] << std::endl;
    return -5;
  }
  std::optional<unsigned int> videostream;
  for (unsigned int i = 0; i < format_context->nb_streams; i++)
  {
    if ((format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) && (format_context->streams[i]->codecpar->codec_id == AVCodecID::AV_CODEC_ID_H264)) // Currently only support H264
    {
      videostream = i;
      break;
    }
  }
  if (!videostream.has_value())
  {
    std::cout << "Failed to find video stream: " << argv[1] << std::endl;
    return -6;
  }
  // Setup window
  std::cout << "Creating window" << std::endl;
  if (!glfwInit())
  {
    std::cout << "Failed to initialise glfw" << std::endl;
    return -7;
  }
  BOOST_SCOPE_EXIT(void)
  {
    glfwTerminate();
  }
  BOOST_SCOPE_EXIT_END
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_DECORATED, true);
  glfwWindowHint(GLFW_RESIZABLE, false);
  GLFWwindow* window = glfwCreateWindow(1024, 768, "Player", nullptr, nullptr);
  if (!window)
  {
    std::cout << "Failed to create window" << std::endl;
    return -8;
  }
  BOOST_SCOPE_EXIT(window)
  {
    glfwDestroyWindow(window);
  }
  BOOST_SCOPE_EXIT_END
  glfwMakeContextCurrent(window);
  BOOST_SCOPE_EXIT(void)
  {
    glfwMakeContextCurrent(nullptr);
  }
  BOOST_SCOPE_EXIT_END
  glfwSwapInterval(1); // Enable vsync
  // EGL functions
  std::cout << "Retrieving EGL functions" << std::endl;
  PFNEGLCREATEIMAGEKHRPROC egl_create_image_khr = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_khr = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  PFNEGLCREATESYNCKHRPROC egl_create_sync_khr = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
  PFNEGLDESTROYSYNCKHRPROC egl_destroy_sync_khr = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
  PFNEGLCLIENTWAITSYNCKHRPROC egl_client_wait_sync_khr = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture_2_does = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if ((egl_create_image_khr == nullptr) || (egl_destroy_image_khr == nullptr) || (egl_create_sync_khr == nullptr) || (egl_destroy_sync_khr == nullptr) || (egl_client_wait_sync_khr == nullptr) || (gl_egl_image_target_texture_2_does == nullptr))
  {
    std::cout << "Failed to retrieve EGL functions" << std::endl;
    return -9;
  }
  // ImGui
  std::cout << "Setting up ImGui" << std::endl;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  BOOST_SCOPE_EXIT(void)
  {
    ImGui::DestroyContext();
  }
  BOOST_SCOPE_EXIT_END
  ImGui::GetIO().IniFilename = nullptr; // Don't load or save settings
  if (ImGui_ImplGlfw_InitForOpenGL(window, true) == false)
  {
    std::cout << "Failed to initialise ImGui" << std::endl;
    return -10;
  }
  ImGui_ImplOpenGL3_Init(GLSL_VERSION_STRING.c_str());
  // Setup OpenGL objects
  std::cout << "Setting up OpenGL objects" << std::endl;
  // Shaders
  const GLuint oes_shader_program = GL_CHECK(glCreateProgram());
  if (oes_shader_program == 0)
  {
    std::cout << "Failed to create OES shader" << std::endl;
    return -11;
  }
  BOOST_SCOPE_EXIT(oes_shader_program)
  {
    GL_CHECK(glDeleteProgram(oes_shader_program));
  }
  BOOST_SCOPE_EXIT_END
  const GLuint shader_program = GL_CHECK(glCreateProgram());
  if (shader_program == 0)
  {
    std::cout << "Failed to create shader" << std::endl;
    return -12;
  }
  BOOST_SCOPE_EXIT(shader_program)
  {
    GL_CHECK(glDeleteProgram(shader_program));
  }
  BOOST_SCOPE_EXIT_END
  if (CreateShader(oes_shader_program, GL_VERTEX_SHADER, vertex_shader.c_str(), vertex_shader.size()))
  {
    std::cout << "Failed to create OES vertex shader" << std::endl;
    return -13;
  }
  if (CreateShader(oes_shader_program, GL_FRAGMENT_SHADER, oes_fragment_shader.c_str(), oes_fragment_shader.size()))
  {
    std::cout << "Failed to create OES pixel shader" << std::endl;
    return -14;
  }
  if (CreateShader(shader_program, GL_VERTEX_SHADER, vertex_shader.c_str(), vertex_shader.size()))
  {
    std::cout << "Failed to create vertex shader" << std::endl;
    return -15;
  }
  if (CreateShader(shader_program, GL_FRAGMENT_SHADER, fragment_shader.c_str(), fragment_shader.size()))
  {
    std::cout << "Failed to create pixel shader" << std::endl;
    return -16;
  }
  // Bind attributes
  GLuint position_location = 0;
  GLuint texture_coord_location = 1;
  GL_CHECK(glBindAttribLocation(oes_shader_program, position_location, "position"));
  GL_CHECK(glBindAttribLocation(oes_shader_program, texture_coord_location, "texcoord"));
  GL_CHECK(glBindAttribLocation(shader_program, position_location, "position"));
  GL_CHECK(glBindAttribLocation(shader_program, texture_coord_location, "texcoord"));
  // Link
  GL_CHECK(glLinkProgram(oes_shader_program));
  GLint result = GL_FALSE;
  GL_CHECK(glGetProgramiv(oes_shader_program, GL_LINK_STATUS, &result));
  if (result == GL_FALSE)
  {
    std::cout << "Failed to link OES shader" << std::endl;
    return -17;
  }
  GL_CHECK(glLinkProgram(shader_program));
  result = GL_FALSE;
  GL_CHECK(glGetProgramiv(shader_program, GL_LINK_STATUS, &result));
  if (result == GL_FALSE)
  {
    std::cout << "Failed to link shader" << std::endl;
    return -18;
  }
  // VAO
  GLuint vao = GL_INVALID_VALUE;
  GL_CHECK(glGenVertexArrays(1, &vao));
  BOOST_SCOPE_EXIT(vao)
  {
    GL_CHECK(glDeleteVertexArrays(1, &vao));
  }
  BOOST_SCOPE_EXIT_END
  GLuint vbo = GL_INVALID_VALUE;
  GL_CHECK(glGenBuffers(1, &vbo));
  BOOST_SCOPE_EXIT(vbo)
  {
    GL_CHECK(glDeleteBuffers(1, &vbo));
  }
  BOOST_SCOPE_EXIT_END
  GLuint ebo = GL_INVALID_VALUE;
  GL_CHECK(glGenBuffers(1, &ebo));
  BOOST_SCOPE_EXIT(ebo)
  {
    GL_CHECK(glDeleteBuffers(1, &ebo));
  }
  BOOST_SCOPE_EXIT_END
  GL_CHECK(glBindVertexArray(vao));
  GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, vbo));
  const float vertices[] =
  {
    // Positions  // Texture coords
    1.0f,  1.0f,  1.0f, 1.0f, // top right
    1.0f,  -1.0f, 1.0f, 0.0f, // bottom right
    -1.0f, -1.0f, 0.0f, 0.0f, // bottom left
    -1.0f, 1.0f,  0.0f, 1.0f  // top left
  };
  GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW));
  GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo));
  const unsigned int indices[] =
  {
    0, 1, 3,
    1, 2, 3
  };
  GL_CHECK(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW));
  // Position attribute
  GL_CHECK(glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0)));
  GL_CHECK(glEnableVertexAttribArray(position_location));
  // Texture coord attribute
  GL_CHECK(glVertexAttribPointer(texture_coord_location, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float))));
  GL_CHECK(glEnableVertexAttribArray(texture_coord_location));
  // Clear up
  GL_CHECK(glBindVertexArray(0));
  GL_CHECK(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
  GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
  GL_CHECK(glDisableVertexAttribArray(position_location));
  GL_CHECK(glDisableVertexAttribArray(texture_coord_location));
  // Retrieve uniforms
  const GLuint oes_texture_sampler_location = GL_CHECK(glGetUniformLocation(oes_shader_program, "tex"));
  if (oes_texture_sampler_location == -1)
  {
    std::cout << "Failed to retrieve OES texture sampler location" << std::endl;
    return -19;
  }
  const GLuint texture_sampler_location = GL_CHECK(glGetUniformLocation(shader_program, "tex"));
  if (texture_sampler_location == -1)
  {
    std::cout << "Failed to retrieve texture sampler location" << std::endl;
    return -20;
  }
  // Setup decoder
  std::cout << "Setting up decoder" << std::endl;
  MppCtx context;
  MppApi* api = nullptr;
  MppPacket packet;
  MppBufferGroup frame_group;
  size_t packet_buffer_size = 64;
  std::unique_ptr<char[]> packet_buffer = std::make_unique<char[]>(packet_buffer_size);
  int ret = mpp_packet_init(&packet, packet_buffer.get(), packet_buffer_size);
  if (ret)
  {
    std::cout << "Failed to initialise MPP packet" << std::endl;
    return -21;
  }
  ret = mpp_create(&context, &api);
  if (ret != MPP_OK)
  {
    std::cout << "Failed to create MPP context" << std::endl;
    return -22;
  }
  MpiCmd mpi_cmd = MPP_DEC_SET_PARSER_SPLIT_MODE;
  RK_U32 need_split = 1;
  MppParam param = &need_split;
  ret = api->control(context, mpi_cmd, param);
  if (ret != MPP_OK)
  {
    std::cout << "Failed to set MPP split mode" << std::endl;
    return  -23;
  }
  ret = mpp_init(context, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
  if (ret != MPP_OK)
  {
    std::cout << "Failed to set MPP H264" << std::endl;
    return -24;
  }
  // Find SPS/PPS if available and pass it to the decoder
  std::vector<uint8_t> spspps;
  if (format_context->streams[*videostream]->codecpar->extradata && format_context->streams[*videostream]->codecpar->extradata_size)
  {
    const std::vector<uint8_t> extradata(format_context->streams[*videostream]->codecpar->extradata, format_context->streams[*videostream]->codecpar->extradata + format_context->streams[*videostream]->codecpar->extradata_size);
    if (extradata.size())
    {
      if (extradata[0] >= 1) // SPS+PPS count, but we only care about the first one
      {
        const int spscount = extradata[5] & 0x1f;
        const int spsnalsize = (extradata[6] << 8) | extradata[7];
        if ((spsnalsize + 8) <= extradata.size())
        {
          std::cout << "Gathering SPS: " << spsnalsize << std::endl;
          spspps.insert(spspps.end(), extradata.data() + 8, extradata.data() + 8 + spsnalsize);
          if ((spsnalsize + 8 + 1) <= extradata.size())
          {
            const int ppscount = extradata[8 + spsnalsize] & 0x1f;
            if (ppscount >= 1)
            {
              if ((spsnalsize + 8 + 1 + 2) < extradata.size())
              {
                const int ppsnalsize = (extradata[8 + spsnalsize + 1] << 8) | extradata[8 + spsnalsize + 2];
                if ((spsnalsize + 8 + 1 + 2 + ppsnalsize) <= extradata.size())
                {
                  std::cout << "Gathering PPS: " << ppsnalsize << std::endl;
                  spspps.insert(spspps.end(), H264_START_SEQUENCE, H264_START_SEQUENCE + sizeof(H264_START_SEQUENCE));
                  spspps.insert(spspps.end(), extradata.data() + 8 + spsnalsize + 3, extradata.data() + 8 + spsnalsize + 3 + ppsnalsize);
                }
              }
            }
          }
        }
      }
    }
  }
  if (spspps.size())
  {
    std::cout << "Sending SPS and PPS" << std::endl;
    if (SendFrame(api, context, spspps.data(), spspps.size(), packet, packet_buffer, packet_buffer_size))
    {
      std::cout << "Failed to send SPS+PPS frame" << std::endl;
      return -25;
    }
  }
  // Start main loop
  std::cout << "Starting main loop" << std::endl;
  MppFrame source_frame = nullptr;
  std::map<MppBuffer, EGL_FRAME> egl_images;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  static double time_base = static_cast<double>(format_context->streams[*videostream]->time_base.num) / static_cast<double>(format_context->streams[*videostream]->time_base.den);
  AVPacket* av_packet = nullptr;
  BOOST_SCOPE_EXIT(&av_packet)
  {
    if (av_packet)
    {
      av_packet_free(&av_packet);
    }
  }
  BOOST_SCOPE_EXIT_END
  std::unique_ptr<FRAME_BUFFER> frame_buffer;
  bool show_window = true;
  boost::optional<MppFrameColorSpace> mpp_colour_space;
  boost::optional<MppFrameColorRange> mpp_colour_range;
  boost::optional<MppFrameColorPrimaries> mpp_colour_primaries;
  int egl_colour_space_override_index = 1;
  int egl_colour_range_override_index = 1;
  while (!glfwWindowShouldClose(window) && running)
  {
    // Calculate time of frame
    if (av_packet)
    {
      const double av_packet_time = static_cast<double>(av_packet->pts) * time_base;
      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      if (now >= (start + std::chrono::milliseconds(static_cast<int>(av_packet_time * 1000.0))))
      {
        av_packet_free(&av_packet);
      }
    }
    // Read frame from file
    if (av_packet == nullptr)
    {
      av_packet = av_packet_alloc();
      ret = av_read_frame(format_context, av_packet);
      if (ret == AVERROR_EOF)
      {
        ret = av_seek_frame(format_context, *videostream, 0, AVSEEK_FLAG_ANY);
        if (ret < 0)
        {
          std::cout << "Failed to seek frame" << std::endl;
          return -26;
        }
        start = std::chrono::steady_clock::now();
      }
      else if (ret)
      {
        std::cout << "Failed to seek frame" << std::endl;
        return -27;
      }
      // Video
      if (av_packet->stream_index != *videostream)
      {
        av_packet_free(&av_packet);
        continue;
      }
      // Send
      const uint8_t* ptr = av_packet->data;
      size_t size = av_packet->size;
      while (size > 5)
      {
        const uint32_t nal_size = htonl(*reinterpret_cast<const uint32_t*>(ptr));
        ptr += 4;
        size -= 4;
        if (nal_size > size)
        {
          std::cout << "Illegal NAL size " << nal_size << std::endl;
          break;
        }
        // Build mpp frame
        if (SendFrame(api, context, ptr, nal_size, packet, packet_buffer, packet_buffer_size))
        {
          std::cout << "Failed to send frame: " << nal_size << std::endl;
          return -28;
        }
        ptr += nal_size;
        size -= nal_size;
      }
    }
    // Collect any output frames
    ret = api->decode_get_frame(context, &source_frame);
    if (ret != MPP_OK)
    {
      std::cout << "Failed to get frame: " << ret << std::endl;
      return -29;
    }
    if (source_frame)
    {
      BOOST_SCOPE_EXIT(source_frame)
      {
        mpp_frame_deinit(&source_frame);
      }
      BOOST_SCOPE_EXIT_END
      if (mpp_frame_get_info_change(source_frame))
      {
        std::cout << "Frame dimensions and format changed" << std::endl;
        ret = mpp_buffer_group_get_internal(&frame_group, MPP_BUFFER_TYPE_DRM);
        if (ret)
        {
          std::cout << "Failed to set buffer group" << std::endl;
          return -30;
        }
        api->control(context, MPP_DEC_SET_EXT_BUF_GROUP, frame_group);
        api->control(context, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      }
      else if (source_frame)
      {
        MppBuffer mpp_buffer = mpp_frame_get_buffer(source_frame);
        if (mpp_buffer == nullptr)
        {
          std::cout << "Failed to retrieve buffer from frame" << std::endl;
          return -31;
        }
        const MppFrameFormat format = mpp_frame_get_fmt(source_frame);
        if (format != MPP_FMT_YUV420SP)
        {
          std::cout << "Invalid frame format" << std::endl;
          return -32;
        }
        mpp_colour_space = mpp_frame_get_colorspace(source_frame);
        mpp_colour_range = mpp_frame_get_color_range(source_frame);
        mpp_colour_primaries = mpp_frame_get_color_primaries(source_frame);
        const RK_U32 width = mpp_frame_get_width(source_frame);
        const RK_U32 height = mpp_frame_get_height(source_frame);
        const RK_U32 offset_x = mpp_frame_get_offset_x(source_frame);
        const RK_U32 offset_y = mpp_frame_get_offset_y(source_frame);
        const RK_U32 hor_stride = mpp_frame_get_hor_stride(source_frame);
        const RK_U32 ver_stride = mpp_frame_get_ver_stride(source_frame);
        std::map<MppBuffer, EGL_FRAME>::iterator e = egl_images.find(mpp_buffer);
        if (e != egl_images.end())
        {
          if ((e->second.colour_space_ != *mpp_colour_space) || (e->second.colour_range_ != *mpp_colour_range) || (e->second.width_ != width) || (e->second.height_ != height))
          {
            std::cout << "MPP buffer format changed, resetting EGL images" << std::endl;
            DestroyEGLFrames(egl_destroy_image_khr, egl_images);
            e = egl_images.end();
          }
        }
        if (e == egl_images.end())
        {
          const int egl_colour_space = EGL_COLOUR_SPACES[egl_colour_space_override_index].first;
          const int egl_colour_range = EGL_COLOUR_RANGES[egl_colour_range_override_index].first;
          // Create EGL image
          const int fd = mpp_buffer_get_fd(mpp_buffer);
          EGLint atts[] = {
                            EGL_WIDTH, static_cast<EGLint>(width),
                            EGL_HEIGHT, static_cast<EGLint>(height),
                            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
                            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                            EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset_x),
                            EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(hor_stride),
                            EGL_DMA_BUF_PLANE1_FD_EXT, fd,
                            EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(offset_x + (hor_stride * ver_stride)),
                            EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(hor_stride),
                            EGL_YUV_COLOR_SPACE_HINT_EXT, egl_colour_space,
                            EGL_SAMPLE_RANGE_HINT_EXT, egl_colour_range,
                            EGL_NONE
                          };
          const EGLImageKHR egl_image = egl_create_image_khr(glfwGetEGLDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, atts);
          if (egl_image == EGL_NO_IMAGE_KHR)
          {
            std::cout << "Failed to create EGL image" << std::endl;
            return -33;
          }
          e = egl_images.insert(std::make_pair(mpp_buffer, EGL_FRAME(egl_image, *mpp_colour_space, *mpp_colour_range, width, height))).first;
        }
        // Create and/or frame buffer
        if (frame_buffer == nullptr)
        {
          GLuint frame = GL_INVALID_VALUE;
          GL_CHECK(glGenFramebuffers(1, &frame));
          GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, frame));
          GLuint frame_buffer_texture = GL_INVALID_VALUE;
          GL_CHECK(glGenTextures(1, &frame_buffer_texture));
          GL_CHECK(glBindTexture(GL_TEXTURE_2D, frame_buffer_texture));
          GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
          GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
          GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
          GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frame_buffer_texture, 0));
          if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
          {
            std::cout << "Failed to create frame buffer" << std::endl;
            return -34;
          }
          frame_buffer = std::make_unique<FRAME_BUFFER>(frame, frame_buffer_texture, width, height);
        }
        else
        {
          GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer->frame_));
        }
        // Draw the EGL buffer
        GL_CHECK(glUseProgram(oes_shader_program));
        // Textures
        GL_CHECK(glActiveTexture(GL_TEXTURE0));
        GL_CHECK(glUniform1i(oes_texture_sampler_location, 0));
        GL_CHECK(gl_egl_image_target_texture_2_does(GL_TEXTURE_EXTERNAL_OES, e->second.image_));
        // Draw elements
        GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
        GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
        GL_CHECK(glBindVertexArray(vao));
        GL_CHECK(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0));
        // Cleanup
        GL_CHECK(glBindVertexArray(0));
        GL_CHECK(glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0));
        GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
        GL_CHECK(glUseProgram(0));
        GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        // Sync
        const EGLSyncKHR egl_sync = egl_create_sync_khr(glfwGetEGLDisplay(), EGL_SYNC_FENCE_KHR, nullptr);
        if (egl_sync == EGL_NO_SYNC_KHR)
        {
          std::cout << "Failed to create EGL sync object" << std::endl;
        }
        else
        {
          if (egl_client_wait_sync_khr(glfwGetEGLDisplay(), egl_sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR) != EGL_CONDITION_SATISFIED_KHR)
          {
            std::cout << "Failed to sync EGL buffers" << std::endl;
          }
          if (egl_destroy_sync_khr(glfwGetEGLDisplay(), egl_sync) == EGL_FALSE)
          {
            std::cout << "Failed to destroy EGL sync object" << std::endl;
          }
        }
      }
    }
    // Poll events
    glfwPollEvents();
    // Clear
    GL_CHECK(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
    // Draw video
    if (frame_buffer)
    {
      // Draw the EGL buffer
      GL_CHECK(glUseProgram(oes_shader_program));
      // Textures
      GL_CHECK(glActiveTexture(GL_TEXTURE0));
      GL_CHECK(glUniform1i(texture_sampler_location, 0));
      GL_CHECK(glBindTexture(GL_TEXTURE_2D, frame_buffer->texture_));
      // Draw elements
      GL_CHECK(glBindVertexArray(vao));
      GL_CHECK(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0));
      // Cleanup
      GL_CHECK(glBindVertexArray(0));
      GL_CHECK(glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0));
      GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
      GL_CHECK(glUseProgram(0));
      GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }
    // ImGui window
    if (show_window)
    {
      bool clear_egl = false;
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ImGui::Begin("Controller", &show_window, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);
      // Color spaces
      ImGui::Text("MPP Colour Space: %s", GetColourSpaceText(mpp_colour_space));
      ImGui::Text("MPP Colour Range: %s", GetColourRangeText(mpp_colour_range));
      ImGui::Text("MPP Colour Primaries: %s", GetColourPrimariesText(mpp_colour_primaries));
      if (ImGui::Combo("EGL Colour Space Override", &egl_colour_space_override_index, [](void*, int index){ return (EGL_COLOUR_SPACES[index].second.data()); }, nullptr, EGL_COLOUR_SPACES.size()))
      {
        clear_egl = true;
      }
      if (ImGui::Combo("EGL Colour Range Override", &egl_colour_range_override_index, [](void*, int index){ return (EGL_COLOUR_RANGES[index].second.data()); }, nullptr, EGL_COLOUR_RANGES.size()))
      {
        clear_egl = true;
      }
      ImGui::End();
      ImGui::EndFrame();
      // ImGui Render
      ImGui::Render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      // Refresh EGL images if we have changed settings
      if (clear_egl)
      {
        DestroyEGLFrames(egl_destroy_image_khr, egl_images);
      }
    }
    // Display render
    glfwSwapBuffers(window);
    // Delay loop
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Clear up
  DestroyEGLFrames(egl_destroy_image_khr, egl_images);
  frame_buffer.reset();
  return 0;
}

