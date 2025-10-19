#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <libgen.h>
#include "config.h"

struct memory {
    char *response;
    size_t size;
};

static size_t write_callback(void *data, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory *mem = (struct memory *)userp;
    
    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(!ptr) return 0;
    
    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;
    
    return realsize;
}

static size_t write_file(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

char* extract_json_string(const char *json, const char *key) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    char *pos = strstr(json, pattern);
    if (!pos) {
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        pos = strstr(json, pattern);
    }
    if (!pos) return NULL;
    
    pos += strlen(pattern);
    char *end = strchr(pos, '"');
    if (!end) return NULL;
    
    size_t len = end - pos;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    
    memcpy(result, pos, len);
    result[len] = '\0';
    return result;
}

int download_file(const char *url, const char *output) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    FILE *fp = fopen(output, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return -1;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS);
    
    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK && http_code == 200) ? 0 : -1;
}

char* fetch_url(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    
    struct memory chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_SECONDS);
    
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || http_code != 200) {
        free(chunk.response);
        return NULL;
    }
    
    return chunk.response;
}

int parse_repo_url(const char *url, char **service, char **owner, char **repo) {
    char *copy = strdup(url);
    if (!copy) return -1;
    
    char *protocol = strstr(copy, "://");
    if (!protocol) {
        free(copy);
        return -1;
    }
    char *host_start = protocol + 3;
    
    char *path_start = strchr(host_start, '/');
    if (!path_start) {
        free(copy);
        return -1;
    }
    *path_start = '\0';
    path_start++;
    
    char *host = host_start;
    
    if (strcmp(host, "github.com") == 0) {
        *service = strdup("github");
    } else if (strcmp(host, "gitlab.com") == 0) {
        *service = strdup("gitlab");
    } else if (strcmp(host, "codeberg.org") == 0) {
        *service = strdup("codeberg");
    } else {
        free(copy);
        return -1;
    }
    
    char *owner_start = path_start;
    char *repo_start = strchr(owner_start, '/');
    if (!repo_start) {
        free(copy);
        free(*service);
        return -1;
    }
    *repo_start = '\0';
    repo_start++;
    
    *owner = strdup(owner_start);
    *repo = strdup(repo_start);
    
    char *git_suffix = strstr(*repo, ".git");
    if (git_suffix) {
        *git_suffix = '\0';
    }
    
    free(copy);
    return 0;
}

int download_release(const char *owner, const char *repo, const char *service) {
    char api_url[512];
    char download_url[512];
    char output_file[256];
    
    if (strcmp(service, "github") == 0) {
        snprintf(api_url, sizeof(api_url), "%s/%s/%s/releases/latest", GITHUB_API, owner, repo);
    } else if (strcmp(service, "gitlab") == 0) {
        char encoded_repo[512];
        char temp[256];
        snprintf(temp, sizeof(temp), "%s%%2F%s", owner, repo);
        CURL *curl = curl_easy_init();
        if (curl) {
            char *output = curl_easy_escape(curl, temp, 0);
            if (output) {
                strncpy(encoded_repo, output, sizeof(encoded_repo));
                curl_free(output);
            }
            curl_easy_cleanup(curl);
        }
        snprintf(api_url, sizeof(api_url), "%s/%s/releases/permalink/latest", GITLAB_API, encoded_repo);
    } else if (strcmp(service, "codeberg") == 0) {
        snprintf(api_url, sizeof(api_url), "%s/%s/%s/releases/latest", CODEBERG_API, owner, repo);
    } else {
        fprintf(stderr, "unknown service: %s\n", service);
        return -1;
    }
    
    printf("checking releases at: %s\n", api_url);
    char *response = fetch_url(api_url);
    if (!response) {
        printf("no releases found, falling back to branch download\n");
        
        if (strcmp(service, "github") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/refs/heads/" DEFAULT_BRANCH "." FALLBACK_FORMAT, GITHUB_URL, owner, repo);
        } else if (strcmp(service, "gitlab") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/-/archive/" DEFAULT_BRANCH "/%s-" DEFAULT_BRANCH "." FALLBACK_FORMAT, GITLAB_URL, owner, repo, repo);
        } else if (strcmp(service, "codeberg") == 0) {
#if CODEBERG_PREFER_TAR_GZ
            const char *format = "tar.gz";
#else
            const char *format = FALLBACK_FORMAT;
#endif
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/" DEFAULT_BRANCH ".%s", CODEBERG_URL, owner, repo, format);
        }
        
        snprintf(output_file, sizeof(output_file), "%s-%s-%s.%s", owner, repo, DEFAULT_BRANCH, FALLBACK_FORMAT);
    } else {
        char *tag_name = extract_json_string(response, "tag_name");
        if (!tag_name) {
            printf("no releases found, falling back to branch download\n");
            free(response);
            
            if (strcmp(service, "github") == 0) {
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/refs/heads/" DEFAULT_BRANCH "." FALLBACK_FORMAT, GITHUB_URL, owner, repo);
            } else if (strcmp(service, "gitlab") == 0) {
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/-/archive/" DEFAULT_BRANCH "/%s-" DEFAULT_BRANCH "." FALLBACK_FORMAT, GITLAB_URL, owner, repo, repo);
            } else if (strcmp(service, "codeberg") == 0) {
#if CODEBERG_PREFER_TAR_GZ
                const char *format = "tar.gz";
#else
                const char *format = FALLBACK_FORMAT;
#endif
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/" DEFAULT_BRANCH ".%s", CODEBERG_URL, owner, repo, format);
            }
            
            snprintf(output_file, sizeof(output_file), "%s-%s-%s.%s", owner, repo, DEFAULT_BRANCH, FALLBACK_FORMAT);
        } else {
            printf("found release: %s\n", tag_name);
            
            if (strcmp(service, "github") == 0) {
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/refs/tags/%s." FALLBACK_FORMAT, GITHUB_URL, owner, repo, tag_name);
            } else if (strcmp(service, "gitlab") == 0) {
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/-/archive/%s/%s-%s." FALLBACK_FORMAT, GITLAB_URL, owner, repo, tag_name, repo, tag_name);
            } else if (strcmp(service, "codeberg") == 0) {
#if CODEBERG_PREFER_TAR_GZ
                const char *format = "tar.gz";
#else
                const char *format = FALLBACK_FORMAT;
#endif
                snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/%s.%s", CODEBERG_URL, owner, repo, tag_name, format);
            }
            
            snprintf(output_file, sizeof(output_file), "%s-%s-%s.%s", owner, repo, tag_name, FALLBACK_FORMAT);
            free(tag_name);
        }
        free(response);
    }
    
    printf("downloading: %s\n", download_url);
    printf("saving as: %s\n", output_file);
    
    int result = download_file(download_url, output_file);
    if (result == 0) {
        printf("download successful: %s\n", output_file);
    } else {
        fprintf(stderr, "download failed\n");
    }
    
    return result;
}

void print_usage(const char *program_name) {
    printf("usage: %s <repository-url>\n", program_name);
    printf("supported services: github, gitlab, codeberg\n");
    printf("examples:\n");
    printf("  %s https://github.com/unixextremist/coreutils\n", program_name);
    printf("  %s https://gitlab.com/username/project\n", program_name);
    printf("  %s https://codeberg.org/owner/repo.git\n", program_name);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *url = argv[1];
    char *service = NULL;
    char *owner = NULL;
    char *repo = NULL;
    
    if (parse_repo_url(url, &service, &owner, &repo) != 0) {
        fprintf(stderr, "error: invalid repository url\n");
        fprintf(stderr, "supported formats:\n");
        fprintf(stderr, "  https://github.com/owner/repo\n");
        fprintf(stderr, "  https://gitlab.com/owner/repo\n");
        fprintf(stderr, "  https://codeberg.org/owner/repo\n");
        return 1;
    }
    
    printf("service: %s, owner: %s, repo: %s\n", service, owner, repo);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = download_release(owner, repo, service);
    curl_global_cleanup();
    
    free(service);
    free(owner);
    free(repo);
    
    return result;
}
