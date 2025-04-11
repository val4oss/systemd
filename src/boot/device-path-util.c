/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "device-path-util.h"
#include "efi-string.h"
#include "util.h"

EFI_STATUS make_multiple_file_device_path(
                EFI_HANDLE device,
                char16_t * const *files,
                EFI_DEVICE_PATH ***ret_dp) {

        EFI_STATUS err;
        EFI_DEVICE_PATH *cur_dp = NULL, **iterator_dp = NULL;
        EFI_DEVICE_PATH *original_device_path = NULL;
        size_t n_files = strv_length(files);

        assert(files);
        assert(ret_dp);

        if (n_files == 0) {
                *ret_dp = NULL;
                return EFI_SUCCESS;
        };

        err = BS->HandleProtocol(device, MAKE_GUID_PTR(EFI_DEVICE_PATH_PROTOCOL),
                                 (void **) &original_device_path);
        if (err != EFI_SUCCESS)
                return err;

        EFI_DEVICE_PATH *end_node = original_device_path;
        while (!device_path_is_end(end_node))
                end_node = device_path_next_node(end_node);

        size_t o_dp_size = (uint8_t *) end_node - (uint8_t *) original_device_path;


        *ret_dp = xnew(EFI_DEVICE_PATH*, n_files + 1);

        iterator_dp = ret_dp[0];

        STRV_FOREACH(file, files) {
                size_t file_size = strsize16(*file);

                /* 1st element: FILEPATH_DEVICE_PATH + path name payload */
                /* 2nd element: DEVICE_PATH_END_NODE */
                *iterator_dp = xnew(EFI_DEVICE_PATH, o_dp_size +
                                    sizeof(FILEPATH_DEVICE_PATH)
                                    + file_size
                                    + sizeof(EFI_DEVICE_PATH));
                cur_dp = *iterator_dp;

                /* Prepend the original device path */
                cur_dp = mempcpy(cur_dp, original_device_path, o_dp_size);

                FILEPATH_DEVICE_PATH *file_dp = (FILEPATH_DEVICE_PATH *) cur_dp;
                file_dp->Header = (EFI_DEVICE_PATH) {
                        .Type = MEDIA_DEVICE_PATH,
                        .SubType = MEDIA_FILEPATH_DP,
                        .Length = sizeof(FILEPATH_DEVICE_PATH) + file_size,
                };
                memcpy(file_dp->PathName, *file, file_size);

                cur_dp = device_path_next_node(cur_dp);
                *cur_dp = DEVICE_PATH_END_NODE;

                iterator_dp++;
        }
        *iterator_dp = NULL;

        return EFI_SUCCESS;
}

EFI_STATUS make_file_device_path(EFI_HANDLE device, const char16_t *file, EFI_DEVICE_PATH **ret_dp) {
        EFI_STATUS err;
        EFI_DEVICE_PATH *dp;

        assert(file);
        assert(ret_dp);

        err = BS->HandleProtocol(device, MAKE_GUID_PTR(EFI_DEVICE_PATH_PROTOCOL), (void **) &dp);
        if (err != EFI_SUCCESS)
                return err;

        EFI_DEVICE_PATH *end_node = dp;
        while (!device_path_is_end(end_node))
                end_node = device_path_next_node(end_node);

        size_t file_size = strsize16(file);
        size_t dp_size = (uint8_t *) end_node - (uint8_t *) dp;

        /* Make a copy that can also hold a file media device path. */
        *ret_dp = xmalloc(dp_size + file_size + sizeof(FILEPATH_DEVICE_PATH) + sizeof(EFI_DEVICE_PATH));
        dp = mempcpy(*ret_dp, dp, dp_size);

        FILEPATH_DEVICE_PATH *file_dp = (FILEPATH_DEVICE_PATH *) dp;
        file_dp->Header = (EFI_DEVICE_PATH) {
                .Type = MEDIA_DEVICE_PATH,
                .SubType = MEDIA_FILEPATH_DP,
                .Length = sizeof(FILEPATH_DEVICE_PATH) + file_size,
        };
        memcpy(file_dp->PathName, file, file_size);

        dp = device_path_next_node(dp);
        *dp = DEVICE_PATH_END_NODE;
        return EFI_SUCCESS;
}

EFI_STATUS make_url_device_path(const char16_t *url, EFI_DEVICE_PATH **ret) {
        assert(url);
        assert(ret);

        /* Turns a URL into a simple one-element URL device path. */

        _cleanup_free_ char* u = xstr16_to_ascii(url);
        if (!u)
                return EFI_INVALID_PARAMETER;

        size_t l = strlen8(u);

        size_t t = offsetof(URI_DEVICE_PATH, Uri) + l + sizeof(EFI_DEVICE_PATH);
        EFI_DEVICE_PATH *dp = xmalloc(t);

        URI_DEVICE_PATH *udp = (URI_DEVICE_PATH*) dp;
        udp->Header = (EFI_DEVICE_PATH) {
                .Type = MESSAGING_DEVICE_PATH,
                .SubType = MSG_URI_DP,
                .Length = offsetof(URI_DEVICE_PATH, Uri) + l,
        };
        memcpy(udp->Uri, u, l);

        EFI_DEVICE_PATH *end = device_path_next_node(dp);
        *end = DEVICE_PATH_END_NODE;

        assert(((uint8_t*) end + sizeof(EFI_DEVICE_PATH)) == ((uint8_t*) dp + t));

        *ret = TAKE_PTR(dp);
        return EFI_SUCCESS;
}

static char16_t *device_path_to_str_internal(const EFI_DEVICE_PATH *dp) {
        char16_t *str = NULL;

        for (const EFI_DEVICE_PATH *node = dp; !device_path_is_end(node); node = device_path_next_node(node)) {
                _cleanup_free_ char16_t *old = str;

                if (node->Type == END_DEVICE_PATH_TYPE && node->SubType == END_INSTANCE_DEVICE_PATH_SUBTYPE) {
                        str = xasprintf("%ls%s,", strempty(old), old ? "\\" : "");
                        continue;
                }

                /* Special-case this so that FilePath-only device path string look and behave nicely. */
                if (node->Type == MEDIA_DEVICE_PATH && node->SubType == MEDIA_FILEPATH_DP) {
                        str = xasprintf("%ls%s%ls",
                                        strempty(old),
                                        old ? "\\" : "",
                                        ((FILEPATH_DEVICE_PATH *) node)->PathName);
                        continue;
                }

                /* Instead of coding all the different types and sub-types here we just use the
                 * generic node form. This function is a best-effort for firmware that does not
                 * provide the EFI_DEVICE_PATH_TO_TEXT_PROTOCOL after all. */

                size_t size = node->Length - sizeof(EFI_DEVICE_PATH);
                _cleanup_free_ char16_t *hex_data = hexdump((uint8_t *) node + sizeof(EFI_DEVICE_PATH), size);
                str = xasprintf("%ls%sPath(%u,%u%s%ls)",
                                strempty(old),
                                old ? "/" : "",
                                node->Type,
                                node->SubType,
                                size == 0 ? "" : ",",
                                hex_data);
        }

        return str;
}

EFI_STATUS device_path_to_str(const EFI_DEVICE_PATH *dp, char16_t **ret) {
        EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dp_to_text;
        EFI_STATUS err;
        _cleanup_free_ char16_t *str = NULL;

        assert(dp);
        assert(ret);

        err = BS->LocateProtocol(MAKE_GUID_PTR(EFI_DEVICE_PATH_TO_TEXT_PROTOCOL), NULL, (void **) &dp_to_text);
        if (err != EFI_SUCCESS) {
                *ret = device_path_to_str_internal(dp);
                return EFI_SUCCESS;
        }

        str = dp_to_text->ConvertDevicePathToText(dp, /* DisplayOnly=*/ false, /* AllowShortcuts= */ false);
        if (!str)
                return EFI_OUT_OF_RESOURCES;

        *ret = TAKE_PTR(str);
        return EFI_SUCCESS;
}

bool device_path_startswith(const EFI_DEVICE_PATH *dp, const EFI_DEVICE_PATH *start) {
        if (!start)
                return true;
        if (!dp)
                return false;
        for (;;) {
                if (device_path_is_end(start))
                        return true;
                if (device_path_is_end(dp))
                        return false;
                if (start->Length != dp->Length)
                        return false;
                if (memcmp(dp, start, start->Length) != 0)
                        return false;
                start = device_path_next_node(start);
                dp = device_path_next_node(dp);
        }
}

EFI_DEVICE_PATH *device_path_replace_node(
                const EFI_DEVICE_PATH *path, const EFI_DEVICE_PATH *node, const EFI_DEVICE_PATH *new_node) {

        /* Create a new device path as a copy of path, while chopping off the remainder starting at the given
         * node. If new_node is provided, it is appended at the end of the new path. */

        assert(path);
        assert(node);

        size_t len = (uint8_t *) node - (uint8_t *) path;
        EFI_DEVICE_PATH *ret = xmalloc(len + (new_node ? new_node->Length : 0) + sizeof(EFI_DEVICE_PATH));
        EFI_DEVICE_PATH *end = mempcpy(ret, path, len);

        if (new_node)
                end = mempcpy(end, new_node, new_node->Length);

        *end = DEVICE_PATH_END_NODE;
        return ret;
}

size_t device_path_size(const EFI_DEVICE_PATH *dp) {
        const EFI_DEVICE_PATH *i = ASSERT_PTR(dp);

        for (; !device_path_is_end(i); i = device_path_next_node(i))
                ;

        return (const uint8_t*) i - (const uint8_t*) dp + sizeof(EFI_DEVICE_PATH);
}
