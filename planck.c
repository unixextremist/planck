#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define USER_AGENT "Mozilla/5.0 (compatible; curl)"
#define TIMEOUT 30L

struct memory { char *response; size_t size; };

static size_t write_cb(void *data, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct memory *mem = userp;
    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(!ptr) return 0;
    mem->response = ptr;
    memcpy(&mem->response[mem->size], data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;
    return realsize;
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
    if (result) {
        memcpy(result, pos, len);
        result[len] = '\0';
    }
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT);
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT);
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
    char *host_start = strstr(copy, "://");
    if (!host_start) goto error;
    host_start += 3;
    char *path_start = strchr(host_start, '/');
    if (!path_start) goto error;
    *path_start = '\0';
    path_start++;
    
    if (strcmp(host_start, "github.com") == 0) *service = strdup("github");
    else if (strcmp(host_start, "gitlab.com") == 0) *service = strdup("gitlab");
    else if (strcmp(host_start, "codeberg.org") == 0) *service = strdup("codeberg");
    else goto error;
    
    char *repo_start = strchr(path_start, '/');
    if (!repo_start) goto error;
    *repo_start = '\0';
    repo_start++;
    *owner = strdup(path_start);
    *repo = strdup(repo_start);
    
    char *git_suffix = strstr(*repo, ".git");
    if (git_suffix) *git_suffix = '\0';
    
    free(copy);
    return 0;
    
error:
    free(copy);
    free(*service);
    return -1;
}

int download_release(const char *owner, const char *repo, const char *service) {
    char api_url[512], download_url[512], output_file[256];
    const char *api_base = NULL, *web_base = NULL;
    
    if (strcmp(service, "github") == 0) {
        api_base = "https://api.github.com/repos";
        web_base = "https://github.com";
    } else if (strcmp(service, "gitlab") == 0) {
        api_base = "https://gitlab.com/api/v4/projects";
        web_base = "https://gitlab.com";
    } else if (strcmp(service, "codeberg") == 0) {
        api_base = "https://codeberg.org/api/v1/repos";
        web_base = "https://codeberg.org";
    } else {
        fprintf(stderr, "unknown service: %s\n", service);
        return -1;
    }
    
    snprintf(api_url, sizeof(api_url), "%s/%s/%s/releases/latest", api_base, owner, repo);
    printf("checking releases at: %s\n", api_url);
    
    char *response = fetch_url(api_url);
    char *tag_name = response ? extract_json_string(response, "tag_name") : NULL;
    
    if (!tag_name) {
        printf("no releases found, falling back to branch download\n");
        const char *branch = "main";
        if (strcmp(service, "github") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/refs/heads/%s.zip", web_base, owner, repo, branch);
        } else if (strcmp(service, "gitlab") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/-/archive/%s/%s-%s.tar.gz", web_base, owner, repo, branch, repo, branch);
        } else {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/%s.tar.gz", web_base, owner, repo, branch);
        }
        snprintf(output_file, sizeof(output_file), "%s-%s-%s.tar.gz", owner, repo, branch);
    } else {
        printf("found release: %s\n", tag_name);
        if (strcmp(service, "github") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/refs/tags/%s.tar.gz", web_base, owner, repo, tag_name);
        } else if (strcmp(service, "gitlab") == 0) {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/-/archive/%s/%s-%s.tar.gz", web_base, owner, repo, tag_name, repo, tag_name);
        } else {
            snprintf(download_url, sizeof(download_url), "%s/%s/%s/archive/%s.tar.gz", web_base, owner, repo, tag_name);
        }
        snprintf(output_file, sizeof(output_file), "%s-%s-%s.tar.gz", owner, repo, tag_name);
        free(tag_name);
    }
    
    free(response);
    printf("downloading: %s\nsaving as: %s\n", download_url, output_file);
    
    int result = download_file(download_url, output_file);
    printf(result == 0 ? "download successful: %s\n" : "download failed\n", output_file);
    return result;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: %s <repository-url>\n", argv[0]);
        return 1;
    }
    
    char *service, *owner, *repo;
    if (parse_repo_url(argv[1], &service, &owner, &repo) != 0) {
        fprintf(stderr, "error: invalid repository url\n");
        return 1;
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int result = download_release(owner, repo, service);
    curl_global_cleanup();
    
    free(service); free(owner); free(repo);
    return result;
}
