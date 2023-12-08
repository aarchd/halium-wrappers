/**
 * GTK 4.12.4+ expects that the underlying GLES implementation would
 * convert textures if a different format is passed to glTexSubImage2D.
 *
 * This actually works in Mesa, but it's out of spec, and breaks
 * glyphs rendering in other implementations (such as NVIDIA, and at
 * least some Adreno devices).
 *
 * This workaround tells GTK that the BGRA8888 format is not supported
 * (note: it's actually supported in most of the devices), so that it
 * can fallback to RGBA which matches the original texture format.
 *
 * Only users of libepoxy are affected by this workaround.
 * 
 * Bug reference: https://gitlab.gnome.org/GNOME/gtk/-/issues/6216
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>

bool epoxy_has_gl_extension(const char *extension)
{
	static bool (*orig_epoxy_call)(const char*) = NULL;
	if (!orig_epoxy_call)
		orig_epoxy_call = dlsym(RTLD_NEXT, "epoxy_has_gl_extension");

	if (strcmp(extension, "GL_EXT_texture_format_BGRA8888") == 0)
		return false;

	return orig_epoxy_call(extension);
}
