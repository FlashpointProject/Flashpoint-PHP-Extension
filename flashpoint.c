#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"
#include "Zend/zend_compile.h"
#include "php_flashpoint.h"

static int (*original_zend_stream_open_function)(const char *filename, zend_file_handle *handle) = NULL;
zif_handler original_readfile;
zif_handler original_file;
zif_handler original_file_get_contents;

#define BUFFER_SIZE 1024

int http_get_and_save(const char* url, const char* file_path, const int path_len) {
    int socket_desc;
    struct sockaddr_in server_addr;
    char server_reply[BUFFER_SIZE];
    char hostname[256];
    char path[256];
    int port = 80;
    
    // Parse URL
    if (sscanf(url, "http://%255[^:/]:%d/%255[^\n]", hostname, &port, path) == 3) {
        // URL with hostname, port, and path
    } else if (sscanf(url, "http://%255[^:/]:%d", hostname, &port) == 2) {
        // URL with hostname and port (no path)
        path[0] = '\0';
    } else if (sscanf(url, "http://%255[^/]/%255[^\n]", hostname, path) == 2) {
        // URL with hostname and path (default port)
    } else if (sscanf(url, "http://%255[^/]", hostname) == 1) {
        // URL with hostname only (default port, no path)
        path[0] = '\0';
    } else {
        php_printf("[Flashpoint - Network] Invalid URL format\n");
        return 1;
    }
    
    // Create socket
    socket_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_desc == -1) {
        php_printf("[Flashpoint - Network] Could not create socket\n");
        return 1;
    }
    
    // Resolve hostname to IP address
    struct hostent *he = gethostbyname(hostname);
    if (he == NULL) {
        php_printf("[Flashpoint - Network] Bad hostname resolution\n");
        return 1;
    }
    
    // Set up the server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)he->h_addr);
    
    // Connect to remote server
    if (connect(socket_desc, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        php_printf("[Flashpoint - Network] Connect error\n");
        return 1;
    }
    
    // Prepare HTTP request
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request),
             "GET /%s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Ignore-PHP: yes\r\n"
             "Connection: close\r\n"
             "\r\n", path, hostname);
    

    // Send HTTP request
    if (send(socket_desc, request, strlen(request), 0) < 0) {
        php_printf("[Flashpoint - Network] Send failed\n");
        return 1;
    }

    int ret;
    char* dirname;
    dirname = estrndup(file_path, path_len);
    php_dirname(dirname, path_len);
    if (*dirname) {
        php_stream_statbuf ssb;
        if (php_stream_stat_path_ex(dirname, PHP_STREAM_URL_STAT_QUIET, &ssb, NULL) < 0) {
                ret = php_stream_mkdir(dirname, 0777,  PHP_STREAM_MKDIR_RECURSIVE, NULL);
                if (!ret) {
                    php_printf("[Flashpoint - Network] Failed to create directory\n");
                    free(dirname);
                    return 1;
                }
                php_printf("[Flashpoint - Network] Created directory: %s\n", dirname);
        }
    } else {
        php_printf("[Flashpoint - Network] Failed to find dirname\n");
        return 1;
    }

    
    // Open file for writing
    FILE *fp = fopen(file_path, "wb");
    if (fp == NULL) {
        php_printf("[Flashpoint - Network] Failed to open file for write\n");
        return 1;
    }
        
    // Receive and save the response
    int bytes_received;
    int header_end = 0;
    char *body_start = NULL;
    int status_code = 0;

    while ((bytes_received = recv(socket_desc, server_reply, BUFFER_SIZE, 0)) > 0) {
        if (!header_end) {
            // Find the status code
            if (status_code == 0) {
                char *status_line = strstr(server_reply, "HTTP/1.");
                if (status_line) {
                    sscanf(status_line, "%*s %d", &status_code);
                    if (status_code != 200) {
                        php_printf("[Flashpoint - Network] Received non-200 status code: %d\n", status_code);
                        fclose(fp);
                        remove(file_path);
                        close(socket_desc);
                        return 1;
                    }
                }
            }

            // Find the end of the header
            body_start = strstr(server_reply, "\r\n\r\n");
            if (body_start) {
                header_end = 1;
                body_start += 4; // Move past "\r\n\r\n"
                fwrite(body_start, 1, bytes_received - (body_start - server_reply), fp);
            }
        } else {
            fwrite(server_reply, 1, bytes_received, fp);
        }
    }
    
    fclose(fp);
    close(socket_desc);

    if (status_code != 200) {
        
        remove(file_path);
        return 1;
    }
    
    php_printf("[Flashpoint - Network] Response received and saved to %s\n", file_path);
    
    return 0;
}

void normalize_slashes(char *path) {
    for (char *p = path; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

char* get_full_path(const char* filename) {
    char* full_path = NULL;

#ifdef _WIN32
    // Windows implementation
    DWORD bufferSize = GetFullPathNameA(filename, 0, NULL, NULL);
    if (bufferSize == 0) {
        fprintf(stderr, "Error getting buffer size: %lu\n", GetLastError());
        return NULL;
    }

    full_path = malloc(bufferSize * sizeof(char));
    if (!full_path) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    if (GetFullPathNameA(filename, bufferSize, full_path, NULL) == 0) {
        fprintf(stderr, "Error getting full path: %lu\n", GetLastError());
        free(full_path);
        return NULL;
    }
#else
    // POSIX implementation
    char* resolved_path = realpath(filename, NULL);
    if (resolved_path == NULL) {
        fprintf(stderr, "Error resolving path: %s\n", strerror(errno));
        return NULL;
    }
    full_path = resolved_path;
#endif

    normalize_slashes(full_path);

    return full_path;
}

size_t write_callback(void *contents, size_t size, size_t nmemb, FILE *file) {
    return fwrite(contents, size, nmemb, file);
}

static int handle_file_access(char *filename, const char* event) {
    char *full_path = get_full_path(filename);
    if (full_path == NULL) {
        php_printf("[Flashpoint] Error getting full path for file: %s\n", filename);
        return -1;
    } else {
        php_printf("[Flashpoint] [%s]: %s\n", event, full_path);
    }
    
    if (access(full_path, F_OK) != -1) {
        php_printf("[Flashpoint] File ready, continuing load\n");
        return 0;
    }

    php_printf("[Flashpoint] File missing, checking if game server has it...\n");

    const char *relative_path = strstr(full_path, "/Legacy/htdocs");
    if (relative_path == NULL) {
        php_printf("Unable to determine relative path.\n");
        return -1;
    }
    relative_path += strlen("/Legacy/htdocs");

    char url[1024];
    snprintf(url, sizeof(url), "http://127.0.0.1:22501/content%s", relative_path);

    php_printf("[Flashpoint] Requesting URL: %s\n", url);

    int result = http_get_and_save(url, full_path, strlen(full_path));

    if (result == 0) {
        php_printf("[Flashpoint] Successfully downloaded file\n");
        return 0;
    } else {
        php_printf("[Flashpoint] Failed to download file\n");
        return -1;
    }
}

static int custom_zend_stream_open(const char *filename, zend_file_handle *handle)
{
    char *copied_name = strcpy(copied_name, filename);
    handle_file_access(copied_name, "require / include"); 
    // Call the original function
    return original_zend_stream_open_function(filename, handle);
}

PHP_FUNCTION(custom_readfile)
{
    char *filename;
	size_t filename_len;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_PATH(filename, filename_len)
	ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    handle_file_access(filename, "readfile");

    original_file(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

PHP_FUNCTION(custom_file)
{
    char *filename;
	size_t filename_len;

	/* Parse arguments */
	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_PATH(filename, filename_len)
	ZEND_PARSE_PARAMETERS_END();

    handle_file_access(filename, "file");

    original_file(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

PHP_FUNCTION(custom_file_get_contents)
{
    char *filename;
    size_t filename_len;

	ZEND_PARSE_PARAMETERS_START(1, 5)
		Z_PARAM_PATH(filename, filename_len)
	ZEND_PARSE_PARAMETERS_END();

    handle_file_access(filename, "file_get_contents");

    // Call the original file_get_contents
    original_file_get_contents(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

PHP_MINIT_FUNCTION(flashpoint)
{
    php_printf("loading flashpoint extension\n");

    // If the ZEND_TSRMLS_CACHE_UPDATE() is in RINIT, move it
    // to MINIT to ensure access to the compiler globals
    #if defined(COMPILE_DL_FLASHPOINT) && defined(ZTS)
        ZEND_TSRMLS_CACHE_UPDATE();
    #endif

    original_zend_stream_open_function = zend_stream_open_function;
    zend_stream_open_function = custom_zend_stream_open;

    zend_function *original;
    original = zend_hash_str_find_ptr(CG(function_table), "file_get_contents", sizeof("file_get_contents")-1);

    if (original != NULL) {
        original_file_get_contents = original->internal_function.handler;
        original->internal_function.handler = PHP_FN(custom_file_get_contents);
    }

    original = zend_hash_str_find_ptr(CG(function_table), "file", sizeof("file")-1);

    if (original != NULL) {
        original_file = original->internal_function.handler;
        original->internal_function.handler = PHP_FN(custom_file);
    }

    original = zend_hash_str_find_ptr(CG(function_table), "readfile", sizeof("readfile")-1);

    if (original != NULL) {
        original_readfile = original->internal_function.handler;
        original->internal_function.handler = PHP_FN(custom_readfile);
    }


    return SUCCESS;
}

PHP_MINFO_FUNCTION(flashpoint)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "flashpoint support", "enabled");
    php_info_print_table_end();
}

const zend_function_entry flashpoint_functions[] = {
    PHP_FE_END
};

zend_module_entry flashpoint_module_entry = {
    STANDARD_MODULE_HEADER,
    "flashpoint",
    flashpoint_functions,
    PHP_MINIT(flashpoint),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(flashpoint),
    "1.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_FLASHPOINT
ZEND_GET_MODULE(flashpoint)
#endif
