/*
 * 蓝光镜像（.iso）识别：给 demux_playlist（展开成 title 条目）与 demux_lavf
 * （识别到时让路）共用。用户 2026-10-26 要求 ISO 当"文件夹 / m3u"容器。
 *
 * 只按扩展名 + libbluray 能否枚举出 title 判定；bd_open 只读 UDF/mpls 元数据，
 * 对 20 GB 镜像也是毫秒级。
 */
#ifndef MP_BLURAY_IMAGE_H
#define MP_BLURAY_IMAGE_H

#include "config.h"

#if HAVE_LIBBLURAY

#include <libbluray/bluray.h>

#include "common/common.h"
#include "misc/bstr.h"
#include "options/path.h"

static inline bool mp_is_bluray_image(bstr url)
{
    if (mp_is_url(url) || !bstr_case_endswith(url, bstr0(".iso")))
        return false;
    char *path = mp_file_get_path(NULL, url);
    if (!path)
        return false;
    BLURAY *bd = bd_open(path, NULL);
    bool ok = false;
    if (bd) {
        ok = bd_get_titles(bd, TITLES_RELEVANT, 0) > 0;
        bd_close(bd);
    }
    talloc_free(path);
    return ok;
}

#else

static inline bool mp_is_bluray_image(bstr url) { return false; }

#endif /* HAVE_LIBBLURAY */

#endif /* MP_BLURAY_IMAGE_H */
