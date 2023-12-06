#include "TextureManager.h"
#include "Bitmap.h"

/*
#if IMGUI_USE_GLBINDING
#define GLBINDING_AVAILABLE 1
#include <glbinding-aux/ContextInfo.h>
#include <glbinding/gl21/gl.h>
#include <glbinding/gl21ext/gl.h>
#include <glbinding/gl30/gl.h>
using namespace gl21;
#elif IMGUI_USE_GLAD2
#include <glad/gl.h>
#else
#include <Windows.h>
#include <gl/GL.h>
#endif
*/

// TODO: reimplemnent glad stuff
#include <Windows.h>
#include <gl/GL.h>

#include <mh/concurrency/thread_sentinel.hpp>
#include <mh/memory/unique_object.hpp>

#include <array>
#include <set>

using namespace tf2_bot_detector;

namespace
{
	struct TextureHandleTraits final
	{
		static void delete_obj(GLuint t)
		{
			glDeleteTextures(1, &t);
		}
		static GLuint release_obj(GLuint& t)
		{
			auto retVal = t;
			t = {};
			return retVal;
		}
		static bool is_obj_valid(GLuint t)
		{
			return t > 0;
		}
	};

	using TextureHandle = mh::unique_object<GLuint, TextureHandleTraits>;

	class TextureManager;

	class Texture final : public ITexture
	{
	public:
		Texture(const TextureManager& manager, const Bitmap& bitmap, const TextureSettings& settings);

		handle_type GetHandle() const override { return m_Handle; }
		const TextureSettings& GetSettings() const override { return m_Settings; }

		uint16_t GetWidth() const override { return m_Width; }
		uint16_t GetHeight() const override { return m_Height; }

	private:
		TextureHandle m_Handle{};
		TextureSettings m_Settings{};
		uint16_t m_Width{};
		uint16_t m_Height{};
	};

	class TextureManager final : public ITextureManager
	{
	public:
		TextureManager();

		void EndFrame() override;
		std::shared_ptr<ITexture> CreateTexture(const Bitmap& bitmap, const TextureSettings& settings) override;
		size_t GetActiveTextureCount() const override { return m_Textures.size(); }

#ifdef IMGUI_USE_GLBINDING
		bool HasExtension(GLextension ext) const { return GetExtensions().contains(ext); }
		const std::set<GLextension>& GetExtensions() const { return m_Extensions; }

		const glbinding::Version& GetContextVersion() const { return m_ContextVersion; }
#endif

	private:
#ifdef IMGUI_USE_GLBINDING
		glbinding::Version m_ContextVersion{};
		const std::set<GLextension> m_Extensions = glbinding::aux::ContextInfo::extensions();
#endif

		uint64_t m_FrameCount{};
		std::vector<std::shared_ptr<Texture>> m_Textures;
		mh::thread_sentinel m_Sentinel;
	};
}

std::shared_ptr<ITextureManager> tf2_bot_detector::ITextureManager::Create()
{
	return std::make_unique<TextureManager>();
}

TextureManager::TextureManager()
{
#ifdef IMGUI_USE_GLBINDING
	m_ContextVersion = glbinding::aux::ContextInfo::version();
#endif
}

void TextureManager::EndFrame()
{
	m_Sentinel.check();
	std::erase_if(m_Textures, [](const std::shared_ptr<Texture>& t)
		{
			return t.use_count() == 1;
		});
}

std::shared_ptr<ITexture> TextureManager::CreateTexture(const Bitmap& bitmap, const TextureSettings& settings)
{
	m_Sentinel.check();
	return m_Textures.emplace_back(std::make_shared<Texture>(*this, bitmap, settings));
}

Texture::Texture(const TextureManager& manager, const Bitmap& bitmap, const TextureSettings& settings) :
	m_Settings(settings),
	m_Width(bitmap.GetWidth()),
	m_Height(bitmap.GetHeight())
{
	GLenum internalFormat{};
	GLenum sourceFormat{};
	std::array<GLint, 4> swizzle{};
	constexpr GLenum sourceType = GL_UNSIGNED_BYTE;
	switch (bitmap.GetChannelCount())
	{
	case 1:
		internalFormat = GL_RED;
		sourceFormat = GL_RED;
		swizzle = { GL_RED, GL_RED, GL_RED, GL_ONE };
		break;
	case 2:
		internalFormat = GL_RGB;
		sourceFormat = GL_RGB;
		swizzle = { GL_RED, GL_RED, GL_RED, GL_GREEN };
		break;
	case 3:
		internalFormat = GL_RGB;
		sourceFormat = GL_RGB;
		swizzle = { GL_RED, GL_GREEN, GL_BLUE, GL_ONE };
		break;
	case 4:
		internalFormat = GL_RGBA;
		sourceFormat = GL_RGBA;
		swizzle = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
		break;
	}

	glGenTextures(1, &m_Handle.reset_and_get_ref());
	assert(m_Handle);

	glBindTexture(GL_TEXTURE_2D, m_Handle);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, bitmap.GetWidth(), bitmap.GetHeight(), 0,
		sourceFormat, sourceType, bitmap.GetData());

#if IMGUI_USE_GLAD2 && FIXME_DISABLE
	if (GLAD_GL_ARB_texture_swizzle)
	{
		glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle.data());
	}
	else if (GLAD_GL_EXT_texture_swizzle)
	{
		glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA_EXT, swizzle.data());
	}
#endif

#if 0
	if (glGenerateMipmap)
	{
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	else if (glGenerateMipmapEXT)
	{
		glGenerateMipmapEXT(GL_TEXTURE_2D);
	}
	else
#endif
	{
		// just disable mipmaps
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
}
