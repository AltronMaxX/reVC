// Nintendo Switch / GLFW movie player. Ported from the SDL2 build; only the
// windowing-system glue differs (GLFW proc address / buffer swap / timer).
#if defined RW_GL3 && !defined LIBRW_SDL2

#include "common.h"

// GLFW_INCLUDE_NONE: don't let GLFW pull its own GL headers - librw already
// provides GL symbols through glad, and GLFWwindow is needed by crossplatform.h.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "MoviePlayer.h"
#include "crossplatform.h"
#include "skeleton.h"

#ifdef AUDIO_OAL
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define PL_MPEG_IMPLEMENTATION
#include "../../../vendor/pl_mpeg/pl_mpeg.h"

typedef PFNGLGENTEXTURESPROC PFN_glGenTextures;
typedef PFNGLBINDTEXTUREPROC PFN_glBindTexture;
typedef PFNGLTEXIMAGE2DPROC PFN_glTexImage2D;
typedef PFNGLTEXPARAMETERIPROC PFN_glTexParameteri;
typedef PFNGLCREATESHADERPROC PFN_glCreateShader;
typedef PFNGLSHADERSOURCEPROC PFN_glShaderSource;
typedef PFNGLCOMPILESHADERPROC PFN_glCompileShader;
typedef PFNGLGETSHADERIVPROC PFN_glGetShaderiv;
typedef PFNGLGETSHADERINFOLOGPROC PFN_glGetShaderInfoLog;
typedef PFNGLCREATEPROGRAMPROC PFN_glCreateProgram;
typedef PFNGLATTACHSHADERPROC PFN_glAttachShader;
typedef PFNGLLINKPROGRAMPROC PFN_glLinkProgram;
typedef PFNGLGETPROGRAMIVPROC PFN_glGetProgramiv;
typedef PFNGLGETPROGRAMINFOLOGPROC PFN_glGetProgramInfoLog;
typedef PFNGLUSEPROGRAMPROC PFN_glUseProgram;
typedef PFNGLGETATTRIBLOCATIONPROC PFN_glGetAttribLocation;
typedef PFNGLGETUNIFORMLOCATIONPROC PFN_glGetUniformLocation;
typedef PFNGLUNIFORM1IPROC PFN_glUniform1i;
typedef PFNGLGENBUFFERSPROC PFN_glGenBuffers;
typedef PFNGLBINDBUFFERPROC PFN_glBindBuffer;
typedef PFNGLBUFFERDATAPROC PFN_glBufferData;
typedef PFNGLVERTEXATTRIBPOINTERPROC PFN_glVertexAttribPointer;
typedef PFNGLENABLEVERTEXATTRIBARRAYPROC PFN_glEnableVertexAttribArray;
typedef PFNGLDISABLEVERTEXATTRIBARRAYPROC PFN_glDisableVertexAttribArray;
typedef PFNGLDRAWARRAYSPROC PFN_glDrawArrays;
typedef PFNGLACTIVETEXTUREPROC PFN_glActiveTexture;
typedef PFNGLVIEWPORTPROC PFN_glViewport;
typedef PFNGLCLEARCOLORPROC PFN_glClearColor;
typedef PFNGLCLEARPROC PFN_glClear;
typedef PFNGLDISABLEPROC PFN_glDisable;
typedef PFNGLGETINTEGERVPROC PFN_glGetIntegerv;
typedef PFNGLGETSTRINGPROC PFN_glGetString;
typedef PFNGLGENVERTEXARRAYSPROC PFN_glGenVertexArrays;
typedef PFNGLBINDVERTEXARRAYPROC PFN_glBindVertexArray;
typedef PFNGLENABLEPROC PFN_glEnable;
typedef PFNGLISENABLEDPROC PFN_glIsEnabled;

namespace
{
	PFN_glGenTextures glGenTextures_ = nullptr;
	PFN_glBindTexture glBindTexture_ = nullptr;
	PFN_glTexImage2D glTexImage2D_ = nullptr;
	PFN_glTexParameteri glTexParameteri_ = nullptr;
	PFN_glCreateShader glCreateShader_ = nullptr;
	PFN_glShaderSource glShaderSource_ = nullptr;
	PFN_glCompileShader glCompileShader_ = nullptr;
	PFN_glGetShaderiv glGetShaderiv_ = nullptr;
	PFN_glGetShaderInfoLog glGetShaderInfoLog_ = nullptr;
	PFN_glCreateProgram glCreateProgram_ = nullptr;
	PFN_glAttachShader glAttachShader_ = nullptr;
	PFN_glLinkProgram glLinkProgram_ = nullptr;
	PFN_glGetProgramiv glGetProgramiv_ = nullptr;
	PFN_glGetProgramInfoLog glGetProgramInfoLog_ = nullptr;
	PFN_glUseProgram glUseProgram_ = nullptr;
	PFN_glGetAttribLocation glGetAttribLocation_ = nullptr;
	PFN_glGetUniformLocation glGetUniformLocation_ = nullptr;
	PFN_glUniform1i glUniform1i_ = nullptr;
	PFN_glGenBuffers glGenBuffers_ = nullptr;
	PFN_glBindBuffer glBindBuffer_ = nullptr;
	PFN_glBufferData glBufferData_ = nullptr;
	PFN_glVertexAttribPointer glVertexAttribPointer_ = nullptr;
	PFN_glEnableVertexAttribArray glEnableVertexAttribArray_ = nullptr;
	PFN_glDisableVertexAttribArray glDisableVertexAttribArray_ = nullptr;
	PFN_glDrawArrays glDrawArrays_ = nullptr;
	PFN_glActiveTexture glActiveTexture_ = nullptr;
	PFN_glViewport glViewport_ = nullptr;
	PFN_glClearColor glClearColor_ = nullptr;
	PFN_glClear glClear_ = nullptr;
	PFN_glDisable glDisable_ = nullptr;
	PFN_glGetIntegerv glGetIntegerv_ = nullptr;
	PFN_glGetString glGetString_ = nullptr;
	// Vertex Array Object entry points. Optional: absent on plain GLES2, but
	// required to draw on a desktop GL core profile (which switch-mesa gives us).
	PFN_glGenVertexArrays glGenVertexArrays_ = nullptr;
	PFN_glBindVertexArray glBindVertexArray_ = nullptr;
	PFN_glEnable glEnable_ = nullptr;
	PFN_glIsEnabled glIsEnabled_ = nullptr;
	bool g_glLoaded = false;

	template<typename T>
	bool LoadOneGL(const char *name, T &fn)
	{
		fn = (T)glfwGetProcAddress(name);
		return fn != nullptr;
	}

	bool LoadGL()
	{
		if (g_glLoaded)
			return true;
		bool ok = true;
		ok &= LoadOneGL("glGenTextures", glGenTextures_);
		ok &= LoadOneGL("glBindTexture", glBindTexture_);
		ok &= LoadOneGL("glTexImage2D", glTexImage2D_);
		ok &= LoadOneGL("glTexParameteri", glTexParameteri_);
		ok &= LoadOneGL("glCreateShader", glCreateShader_);
		ok &= LoadOneGL("glShaderSource", glShaderSource_);
		ok &= LoadOneGL("glCompileShader", glCompileShader_);
		ok &= LoadOneGL("glGetShaderiv", glGetShaderiv_);
		ok &= LoadOneGL("glGetShaderInfoLog", glGetShaderInfoLog_);
		ok &= LoadOneGL("glCreateProgram", glCreateProgram_);
		ok &= LoadOneGL("glAttachShader", glAttachShader_);
		ok &= LoadOneGL("glLinkProgram", glLinkProgram_);
		ok &= LoadOneGL("glGetProgramiv", glGetProgramiv_);
		ok &= LoadOneGL("glGetProgramInfoLog", glGetProgramInfoLog_);
		ok &= LoadOneGL("glUseProgram", glUseProgram_);
		ok &= LoadOneGL("glGetAttribLocation", glGetAttribLocation_);
		ok &= LoadOneGL("glGetUniformLocation", glGetUniformLocation_);
		ok &= LoadOneGL("glUniform1i", glUniform1i_);
		ok &= LoadOneGL("glGenBuffers", glGenBuffers_);
		ok &= LoadOneGL("glBindBuffer", glBindBuffer_);
		ok &= LoadOneGL("glBufferData", glBufferData_);
		ok &= LoadOneGL("glVertexAttribPointer", glVertexAttribPointer_);
		ok &= LoadOneGL("glEnableVertexAttribArray", glEnableVertexAttribArray_);
		ok &= LoadOneGL("glDisableVertexAttribArray", glDisableVertexAttribArray_);
		ok &= LoadOneGL("glDrawArrays", glDrawArrays_);
		ok &= LoadOneGL("glActiveTexture", glActiveTexture_);
		ok &= LoadOneGL("glViewport", glViewport_);
		ok &= LoadOneGL("glClearColor", glClearColor_);
		ok &= LoadOneGL("glClear", glClear_);
		ok &= LoadOneGL("glDisable", glDisable_);
		ok &= LoadOneGL("glGetIntegerv", glGetIntegerv_);
		ok &= LoadOneGL("glGetString", glGetString_);
		ok &= LoadOneGL("glEnable", glEnable_);
		ok &= LoadOneGL("glIsEnabled", glIsEnabled_);
		// VAO funcs are optional (GLES2 has none) - don't fail LoadGL if missing.
		LoadOneGL("glGenVertexArrays", glGenVertexArrays_);
		LoadOneGL("glBindVertexArray", glBindVertexArray_);
		g_glLoaded = ok;
		if (!g_glLoaded)
			printf("MoviePlayer: failed to resolve one or more GL functions via glfwGetProcAddress\n");
		return g_glLoaded;
	}
}

namespace
{
	enum class State { Inactive, Active, Stopping };
	State g_state = State::Inactive;
	bool g_paused = false;

	plm_t *g_plm = nullptr;
	uint8_t *g_rgbaBuffer = nullptr;
	int g_videoWidth = 0;
	int g_videoHeight = 0;
	double g_frameDuration = 1.0 / 30.0; // seconds per video frame, from plm_get_framerate()

	bool g_audioReady = false;
	int g_audioSampleRate = 48000;
	double g_audioFrameDuration = (double)PLM_AUDIO_SAMPLES_PER_FRAME / 48000.0; // seconds per audio frame
	double g_audioAccumulator = 0.0;

#ifdef AUDIO_OAL
	ALCdevice *g_alDevice = nullptr;
	ALCcontext *g_alContext = nullptr;
	ALuint g_alSource = 0;
	static const int kNumAudioBuffers = 4;
	ALuint g_alBuffers[kNumAudioBuffers] = { 0, 0, 0, 0 };
	int g_nextFreeBuffer = 0;
#endif

	double g_lastTime = 0.0; // seconds, from glfwGetTime()
	double g_accumulator = 0.0;

#ifdef AUDIO_OAL
	bool InitAudio()
	{
		g_alDevice = alcOpenDevice(nullptr); // default device
		if (!g_alDevice) {
			printf("MoviePlayer: alcOpenDevice failed, continuing without audio\n");
			return false;
		}

		g_alContext = alcCreateContext(g_alDevice, nullptr);
		if (!g_alContext) {
			printf("MoviePlayer: alcCreateContext failed, continuing without audio\n");
			alcCloseDevice(g_alDevice);
			g_alDevice = nullptr;
			return false;
		}

		alcMakeContextCurrent(g_alContext);

		alGenSources(1, &g_alSource);
		alGenBuffers(kNumAudioBuffers, g_alBuffers);

		g_nextFreeBuffer = 0;
		g_audioReady = true;
		return true;
	}

	void CloseAudio()
	{
		if (!g_audioReady)
			return;

		if (g_alSource) {
			alSourceStop(g_alSource);
			ALint queued = 0;
			alGetSourcei(g_alSource, AL_BUFFERS_QUEUED, &queued);
			while (queued-- > 0) {
				ALuint buf;
				alSourceUnqueueBuffers(g_alSource, 1, &buf);
			}
			alDeleteSources(1, &g_alSource);
			g_alSource = 0;
		}

		alDeleteBuffers(kNumAudioBuffers, g_alBuffers);
		for (int i = 0; i < kNumAudioBuffers; i++)
			g_alBuffers[i] = 0;

		alcMakeContextCurrent(nullptr);
		if (g_alContext) { alcDestroyContext(g_alContext); g_alContext = nullptr; }
		if (g_alDevice) { alcCloseDevice(g_alDevice); g_alDevice = nullptr; }

		g_audioReady = false;
	}

	void QueueAudioFrame(plm_samples_t *samples)
	{
		if (!g_audioReady)
			return;

		static int16_t pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
		for (int i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
			float s = samples->interleaved[i];
			if (s > 1.0f) s = 1.0f;
			if (s < -1.0f) s = -1.0f;
			pcm[i] = (int16_t)(s * 32767.0f);
		}

		ALuint buffer = 0;
		ALint processed = 0;
		alGetSourcei(g_alSource, AL_BUFFERS_PROCESSED, &processed);
		if (processed > 0) {
			alSourceUnqueueBuffers(g_alSource, 1, &buffer);
		} else {
			if (g_nextFreeBuffer >= kNumAudioBuffers)
				return;
			buffer = g_alBuffers[g_nextFreeBuffer++];
		}

		alBufferData(buffer, AL_FORMAT_STEREO16, pcm, sizeof(pcm), (ALsizei)g_audioSampleRate);
		alSourceQueueBuffers(g_alSource, 1, &buffer);

		ALint state = 0;
		alGetSourcei(g_alSource, AL_SOURCE_STATE, &state);
		if (state != AL_PLAYING)
			alSourcePlay(g_alSource);
	}
#else
	bool InitAudio() { return false; }
	void CloseAudio() {}
	void QueueAudioFrame(plm_samples_t *) {}
#endif

	void CloseMovie()
	{
		if (g_plm) { plm_destroy(g_plm); g_plm = nullptr; }
		if (g_rgbaBuffer) { free(g_rgbaBuffer); g_rgbaBuffer = nullptr; }
		CloseAudio();
		g_videoWidth = 0;
		g_videoHeight = 0;
		g_paused = false;
		g_state = State::Inactive;
	}

	// Two shader variants. librw hands us either a desktop GL core context
	// (switch-mesa gives GL 3.3 core) or a GLES context, and the two speak
	// different GLSL dialects. Without an explicit #version the source defaults
	// to GLSL 110, which a strict core-profile driver rejects - that is why the
	// movie played with sound but no picture. Pick the dialect at runtime.
	const char *kVertexShaderGL =
		"#version 150\n"
		"in vec2 aPos;\n"
		"in vec2 aTexCoord;\n"
		"out vec2 vTexCoord;\n"
		"void main() {\n"
		"    vTexCoord = aTexCoord;\n"
		"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
		"}\n";

	const char *kFragmentShaderGL =
		"#version 150\n"
		"in vec2 vTexCoord;\n"
		"out vec4 FragColor;\n"
		"uniform sampler2D uTexture;\n"
		"void main() {\n"
		"    FragColor = texture(uTexture, vTexCoord);\n"
		"}\n";

	const char *kVertexShaderES =
		"#version 100\n"
		"attribute vec2 aPos;\n"
		"attribute vec2 aTexCoord;\n"
		"varying vec2 vTexCoord;\n"
		"void main() {\n"
		"    vTexCoord = aTexCoord;\n"
		"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
		"}\n";

	const char *kFragmentShaderES =
		"#version 100\n"
		"precision mediump float;\n"
		"varying vec2 vTexCoord;\n"
		"uniform sampler2D uTexture;\n"
		"void main() {\n"
		"    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
		"}\n";

	GLuint g_program = 0;
	GLuint g_vbo = 0;
	GLuint g_vao = 0;
	GLuint g_texture = 0;
	GLint g_aPosLoc = -1;
	GLint g_aTexCoordLoc = -1;
	GLint g_uTextureLoc = -1;
	bool g_shaderReady = false;

	GLuint CompileShader(GLenum type, const char *src)
	{
		GLuint shader = glCreateShader_(type);
		glShaderSource_(shader, 1, &src, nullptr);
		glCompileShader_(shader);
		GLint status = 0;
		glGetShaderiv_(shader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE) {
			char log[512];
			glGetShaderInfoLog_(shader, sizeof(log), nullptr, log);
			printf("MoviePlayer: shader compile error: %s\n", log);
		}
		return shader;
	}

	bool InitShader()
	{
		if (g_shaderReady)
			return true;

		const char *ver = glGetString_ ? (const char *)glGetString_(GL_VERSION) : nullptr;
		bool isGLES = ver && strstr(ver, "OpenGL ES") != nullptr;

		GLuint vs = CompileShader(GL_VERTEX_SHADER, isGLES ? kVertexShaderES : kVertexShaderGL);
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, isGLES ? kFragmentShaderES : kFragmentShaderGL);

		g_program = glCreateProgram_();
		glAttachShader_(g_program, vs);
		glAttachShader_(g_program, fs);
		glLinkProgram_(g_program);

		GLint linkStatus = 0;
		glGetProgramiv_(g_program, GL_LINK_STATUS, &linkStatus);
		if (linkStatus == GL_FALSE) {
			char log[512];
			glGetProgramInfoLog_(g_program, sizeof(log), nullptr, log);
			printf("MoviePlayer: shader link error: %s\n", log);
			return false;
		}

		g_aPosLoc = glGetAttribLocation_(g_program, "aPos");
		g_aTexCoordLoc = glGetAttribLocation_(g_program, "aTexCoord");
		g_uTextureLoc = glGetUniformLocation_(g_program, "uTexture");

		const GLfloat verts[] = {
			// x,    y,     u,    v
			-1.0f, -1.0f,  0.0f, 1.0f,
			 1.0f, -1.0f,  1.0f, 1.0f,
			-1.0f,  1.0f,  0.0f, 0.0f,
			 1.0f,  1.0f,  1.0f, 0.0f,
		};

		// A core-profile context can't draw without a bound VAO, and binding our
		// own also stops us from trampling the VAO librw left bound.
		if (glGenVertexArrays_ && glBindVertexArray_)
			glGenVertexArrays_(1, &g_vao);

		glGenBuffers_(1, &g_vbo);
		glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);
		glBufferData_(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

		g_shaderReady = true;
		return true;
	}

	void DrawCurrentFrame()
	{
		if (!LoadGL() || g_rgbaBuffer == nullptr)
			return;

		// Save every GL state we are about to touch. librw keeps a render-state
		// cache that mirrors what it last set; the current GL state IS that cache.
		// If we leave GL changed, librw skips the "redundant" re-set and renders
		// nothing after the movie (menu invisible, last frame frozen). Restoring
		// GL to what we found keeps librw's cache valid.
		GLint sProgram = 0, sArrayBuf = 0, sVao = 0, sActiveTex = 0, sTexBind = 0;
		GLint sViewport[4] = { 0, 0, 0, 0 };
		glGetIntegerv_(GL_CURRENT_PROGRAM, &sProgram);
		glGetIntegerv_(GL_ARRAY_BUFFER_BINDING, &sArrayBuf);
		if (glBindVertexArray_)
			glGetIntegerv_(GL_VERTEX_ARRAY_BINDING, &sVao);
		glGetIntegerv_(GL_ACTIVE_TEXTURE, &sActiveTex);
		glGetIntegerv_(GL_VIEWPORT, sViewport);
		unsigned char sDepth = glIsEnabled_(GL_DEPTH_TEST);
		unsigned char sCull  = glIsEnabled_(GL_CULL_FACE);
		unsigned char sBlend = glIsEnabled_(GL_BLEND);

		glActiveTexture_(GL_TEXTURE0);
		glGetIntegerv_(GL_TEXTURE_BINDING_2D, &sTexBind);

		if (g_texture == 0)
			glGenTextures_(1, &g_texture);

		glBindTexture_(GL_TEXTURE_2D, g_texture);
		glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, g_videoWidth, g_videoHeight,
			0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgbaBuffer);
		glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		if (!InitShader()) {
			// Restore the texture binding we disturbed before bailing.
			glBindTexture_(GL_TEXTURE_2D, (GLuint)sTexBind);
			glActiveTexture_((GLenum)sActiveTex);
			return;
		}

		// Size the viewport from the actual framebuffer. At this early boot stage
		// the cached GL_VIEWPORT may still be 0x0 (nothing has rendered yet).
		int fbWidth = 0, fbHeight = 0;
		glfwGetFramebufferSize(PSGLOBAL(window), &fbWidth, &fbHeight);
		glViewport_(0, 0, fbWidth, fbHeight);

		glDisable_(GL_DEPTH_TEST);
		glDisable_(GL_CULL_FACE);
		glDisable_(GL_BLEND);

		glClearColor_(0.0f, 0.0f, 0.0f, 1.0f);
		glClear_(GL_COLOR_BUFFER_BIT);

		glUseProgram_(g_program);

		if (glBindVertexArray_)
			glBindVertexArray_(g_vao);

		glBindTexture_(GL_TEXTURE_2D, g_texture);
		glUniform1i_(g_uTextureLoc, 0);

		glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);

		glEnableVertexAttribArray_((GLuint)g_aPosLoc);
		glVertexAttribPointer_((GLuint)g_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);

		glEnableVertexAttribArray_((GLuint)g_aTexCoordLoc);
		glVertexAttribPointer_((GLuint)g_aTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));

		glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);

		glDisableVertexAttribArray_((GLuint)g_aPosLoc);
		glDisableVertexAttribArray_((GLuint)g_aTexCoordLoc);

		glfwSwapBuffers(PSGLOBAL(window));

		// Restore everything so librw's state cache stays in sync.
		if (glBindVertexArray_)
			glBindVertexArray_((GLuint)sVao);
		glUseProgram_((GLuint)sProgram);
		glBindBuffer_(GL_ARRAY_BUFFER, (GLuint)sArrayBuf);
		glBindTexture_(GL_TEXTURE_2D, (GLuint)sTexBind);
		glActiveTexture_((GLenum)sActiveTex);
		glViewport_(sViewport[0], sViewport[1], sViewport[2], sViewport[3]);
		if (sDepth) glEnable_(GL_DEPTH_TEST);
		if (sCull)  glEnable_(GL_CULL_FACE);
		if (sBlend) glEnable_(GL_BLEND);
	}
}

void MoviePlayer::Play(const char *path)
{
	if (g_state != State::Inactive)
		CloseMovie();

	char *realPath = nil;
#ifndef _WIN32
	realPath = casepath(path);
#endif
	const char *openPath = realPath ? realPath : path;

	g_plm = plm_create_with_filename(openPath);
	if (realPath)
		free(realPath);

	if (!g_plm) {
		printf("MoviePlayer: failed to open %s\n", path);
		return;
	}

	if (!plm_has_headers(g_plm)) {
		printf("MoviePlayer: %s has no valid MPEG-PS headers\n", path);
		plm_destroy(g_plm);
		g_plm = nullptr;
		return;
	}

	g_videoWidth = plm_get_width(g_plm);
	g_videoHeight = plm_get_height(g_plm);

	if (g_videoWidth <= 0 || g_videoHeight <= 0) {
		printf("MoviePlayer: %s has no video stream\n", path);
		plm_destroy(g_plm);
		g_plm = nullptr;
		return;
	}

	g_rgbaBuffer = (uint8_t *)malloc((size_t)g_videoWidth * (size_t)g_videoHeight * 4);
	g_frameDuration = 1.0 / plm_get_framerate(g_plm);

	g_audioSampleRate = plm_get_samplerate(g_plm);
	if (g_audioSampleRate > 0 && InitAudio()) {
		plm_set_audio_enabled(g_plm, TRUE);
		plm_set_audio_stream(g_plm, 0);
		g_audioFrameDuration = (double)PLM_AUDIO_SAMPLES_PER_FRAME / (double)g_audioSampleRate;
		g_audioAccumulator = 0.0;
	} else {
		plm_set_audio_enabled(g_plm, FALSE);
	}

	printf("MoviePlayer: playing %s (%dx%d, %.2f fps, duration %.2fs, audio=%s)\n",
		path, g_videoWidth, g_videoHeight, plm_get_framerate(g_plm), plm_get_duration(g_plm),
		g_audioReady ? "yes" : "no");

	g_lastTime = glfwGetTime();
	g_accumulator = 0.0;
	g_paused = false;
	g_state = State::Active;
}

bool MoviePlayer::IsActive()
{
	return g_state != State::Inactive;
}

void MoviePlayer::SetPaused(bool paused)
{
	if (g_state == State::Inactive || g_paused == paused)
		return;

	g_paused = paused;
#ifdef AUDIO_OAL
	if (g_audioReady) {
		if (paused)
			alSourcePause(g_alSource);
		else
			alSourcePlay(g_alSource);
	}
#endif
	if (!paused)
		g_lastTime = glfwGetTime();
}

void MoviePlayer::Stop()
{
	if (g_state == State::Active)
		g_state = State::Stopping;
}

void MoviePlayer::Draw()
{
	if (g_state == State::Inactive || g_paused)
		return;

	if (g_state == State::Stopping) {
		CloseMovie();
		return;
	}

	double now = glfwGetTime();
	double elapsed = now - g_lastTime;
	g_lastTime = now;
	g_accumulator += elapsed;

	const double kMaxAccumulator = g_frameDuration * 4.0;
	if (g_accumulator > kMaxAccumulator)
		g_accumulator = kMaxAccumulator;

	const int kMaxFramesPerCall = 4;
	int framesDecoded = 0;
	while (g_accumulator >= g_frameDuration && framesDecoded < kMaxFramesPerCall) {
		plm_frame_t *frame = plm_decode_video(g_plm);
		if (frame == nullptr)
			break;

		plm_frame_to_rgba(frame, g_rgbaBuffer, g_videoWidth * 4);
		DrawCurrentFrame();

		g_accumulator -= g_frameDuration;
		framesDecoded++;
	}

	if (g_audioReady) {
		g_audioAccumulator += elapsed;
		const double kMaxAudioAccumulator = g_audioFrameDuration * 8.0;
		if (g_audioAccumulator > kMaxAudioAccumulator)
			g_audioAccumulator = kMaxAudioAccumulator;

		const int kMaxAudioFramesPerCall = 8;
		int audioFramesDecoded = 0;
		while (g_audioAccumulator >= g_audioFrameDuration && audioFramesDecoded < kMaxAudioFramesPerCall) {
			plm_samples_t *samples = plm_decode_audio(g_plm);
			if (samples == nullptr)
				break;
			QueueAudioFrame(samples);
			g_audioAccumulator -= g_audioFrameDuration;
			audioFramesDecoded++;
		}
	}

	if (plm_has_ended(g_plm)) {
		printf("MoviePlayer: movie finished\n");
		g_state = State::Stopping;
	}
}

#endif // RW_GL3 && !LIBRW_SDL2
