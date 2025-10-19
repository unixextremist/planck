#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <stdarg.h>

#define MAX_PATH 4096
#define BUFFER_SIZE 8192

typedef enum {
    REPO_GITHUB,
    REPO_GITLAB,
    REPO_CODEBERG,
    REPO_GENERIC
} RepoType;

typedef struct {
    char url[1024];
    char path[MAX_PATH];
    char branch[64];
    int verbose;
    int use_https;
} CloneOptions;

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

void log_message(int verbose, const char* format, ...) {
    if (!verbose) return;
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

int create_directory(const char* path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            perror("mkdir failed");
            return -1;
        }
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Path exists but is not a directory: %s\n", path);
        return -1;
    }
    return 0;
}

void get_repo_name(const char* url, char* name, size_t name_size) {
    const char* last_slash = strrchr(url, '/');
    const char* last_dot = strrchr(url, '.');
    
    if (!last_slash) {
        strncpy(name, "repository", name_size);
        name[name_size-1] = '\0';
        return;
    }
    
    const char* start = last_slash + 1;
    const char* end = last_dot && last_dot > start ? last_dot : url + strlen(url);
    
    size_t len = end - start;
    if (len >= name_size) len = name_size - 1;
    
    strncpy(name, start, len);
    name[len] = '\0';
    
    size_t current_len = strlen(name);
    if (current_len > 4 && strcmp(name + current_len - 4, ".git") == 0) {
        name[current_len - 4] = '\0';
    }
}

int http_download_safe(const char* url, const char* output_path, int verbose) {
    CURL *curl = curl_easy_init();
    if(!curl) {
        log_message(verbose, "Failed to initialize CURL\n");
        return -1;
    }
    
    FILE *fp = fopen(output_path, "wb");
    if(!fp) {
        log_message(verbose, "Failed to open output file: %s\n", output_path);
        curl_easy_cleanup(curl);
        return -1;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "simple-clone/1.0");
    
    log_message(verbose, "Downloading: %s\n", url);
    CURLcode res = curl_easy_perform(curl);
    
    fclose(fp);
    curl_easy_cleanup(curl);
    
    if(res != CURLE_OK) {
        log_message(verbose, "Download failed: %s\n", curl_easy_strerror(res));
        return -1;
    }
    
    return 0;
}

RepoType detect_repo_type(const char* url) {
    if (strstr(url, "github.com")) return REPO_GITHUB;
    if (strstr(url, "gitlab.com")) return REPO_GITLAB;
    if (strstr(url, "codeberg.org")) return REPO_CODEBERG;
    return REPO_GENERIC;
}

int extract_archive(const char* archive_path, const char* extract_path, int verbose) {
    pid_t pid = fork();
    if (pid == 0) {
        const char* args_tar[] = {"tar", "-xf", archive_path, "-C", extract_path, "--strip-components=1", NULL};
        const char* args_unzip[] = {"unzip", "-q", archive_path, "-d", extract_path, NULL};
        
        execvp("tar", (char* const*)args_tar);
        execvp("unzip", (char* const*)args_unzip);
        
        fprintf(stderr, "Failed to extract archive: need tar or unzip\n");
        exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                log_message(verbose, "Successfully extracted archive\n");
            } else {
                log_message(verbose, "Archive extraction failed with code %d\n", exit_code);
            }
            return exit_code;
        }
        return -1;
    }
    perror("fork failed");
    return -1;
}

int git_init(const char* path, int verbose) {
    char git_dir[MAX_PATH];
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    
    if (create_directory(git_dir) != 0) {
        log_message(verbose, "Failed to create .git directory\n");
        return -1;
    }
    
    char subdir[MAX_PATH];
    const char* subdirs[] = {"objects", "refs", "refs/heads", "refs/tags", "info", "hooks", NULL};
    
    for (int i = 0; subdirs[i]; i++) {
        snprintf(subdir, sizeof(subdir), "%s/%s", git_dir, subdirs[i]);
        if (create_directory(subdir) != 0) {
            log_message(verbose, "Failed to create subdirectory: %s\n", subdir);
            return -1;
        }
    }
    
    char head_file[MAX_PATH];
    snprintf(head_file, sizeof(head_file), "%s/HEAD", git_dir);
    FILE* fp = fopen(head_file, "w");
    if (fp) {
        fprintf(fp, "ref: refs/heads/main\n");
        fclose(fp);
    } else {
        log_message(verbose, "Failed to create HEAD file\n");
        return -1;
    }
    
    char config_file[MAX_PATH];
    snprintf(config_file, sizeof(config_file), "%s/config", git_dir);
    fp = fopen(config_file, "w");
    if (fp) {
        fprintf(fp, "[core]\n");
        fprintf(fp, "\trepositoryformatversion = 0\n");
        fprintf(fp, "\tfilemode = true\n");
        fprintf(fp, "\tbare = false\n");
        fprintf(fp, "\tlogallrefupdates = true\n");
        fclose(fp);
    } else {
        log_message(verbose, "Failed to create config file\n");
        return -1;
    }
    
    log_message(verbose, "Initialized git repository\n");
    return 0;
}

int detect_default_branch(const char* url, char* branch, size_t branch_size, int verbose) {
    RepoType repo_type = detect_repo_type(url);
    
    if (repo_type == REPO_GITHUB) {
        char api_url[2048];
        const char* github_prefix = "https://github.com/";
        if (strstr(url, github_prefix)) {
            const char* repo_path = url + strlen(github_prefix);
            char owner[256], repo[256];
            if (sscanf(repo_path, "%255[^/]/%255[^.]", owner, repo) == 2) {
                snprintf(api_url, sizeof(api_url), 
                        "https://api.github.com/repos/%s/%s", owner, repo);
                
                char temp_file[MAX_PATH];
                snprintf(temp_file, sizeof(temp_file), "/tmp/clone_temp_%d.json", getpid());
                
                if (http_download_safe(api_url, temp_file, verbose) == 0) {
                    FILE* fp = fopen(temp_file, "r");
                    if (fp) {
                        char line[256];
                        while (fgets(line, sizeof(line), fp)) {
                            if (strstr(line, "\"default_branch\"")) {
                                char* start = strchr(line, ':');
                                if (start) {
                                    start++;
                                    while (*start == ' ' || *start == '"') start++;
                                    char* end = strchr(start, '"');
                                    if (end) {
                                        size_t len = end - start;
                                        if (len < branch_size) {
                                            strncpy(branch, start, len);
                                            branch[len] = '\0';
                                            fclose(fp);
                                            unlink(temp_file);
                                            log_message(verbose, "Detected default branch: %s\n", branch);
                                            return 0;
                                        }
                                    }
                                }
                            }
                        }
                        fclose(fp);
                    }
                    unlink(temp_file);
                }
            }
        }
    }
    
    strncpy(branch, "main", branch_size);
    log_message(verbose, "Using default branch: %s\n", branch);
    return 0;
}

int clone_via_archive(const CloneOptions* opts) {
    if (create_directory(opts->path) != 0) {
        return -1;
    }
    
    if (git_init(opts->path, opts->verbose) != 0) {
        return -1;
    }
    
    char repo_name[256];
    get_repo_name(opts->url, repo_name, sizeof(repo_name));
    
    char branch[64];
    strncpy(branch, opts->branch, sizeof(branch));
    if (strlen(branch) == 0) {
        detect_default_branch(opts->url, branch, sizeof(branch), opts->verbose);
    }
    
    char archive_url[2048];
    RepoType repo_type = detect_repo_type(opts->url);
    
    switch (repo_type) {
        case REPO_CODEBERG: {
            const char* codeberg_prefix = "https://codeberg.org/";
            const char* repo_path = opts->url + strlen(codeberg_prefix);
            snprintf(archive_url, sizeof(archive_url), 
                    "https://codeberg.org/%s/archive/%s.tar.gz", 
                    repo_path, branch);
            break;
        }
        case REPO_GITHUB: {
            const char* github_prefix = "https://github.com/";
            const char* repo_path = opts->url + strlen(github_prefix);
            snprintf(archive_url, sizeof(archive_url), 
                    "https://github.com/%s/archive/refs/heads/%s.tar.gz", 
                    repo_path, branch);
            break;
        }
        case REPO_GITLAB: {
            const char* gitlab_prefix = "https://gitlab.com/";
            const char* repo_path = opts->url + strlen(gitlab_prefix);
            snprintf(archive_url, sizeof(archive_url), 
                    "https://gitlab.com/%s/-/archive/%s/%s-%s.tar.gz", 
                    repo_path, branch, repo_name, branch);
            break;
        }
        default: {
            snprintf(archive_url, sizeof(archive_url), "%s/archive/%s.tar.gz", opts->url, branch);
            break;
        }
    }
    
    char temp_archive[MAX_PATH];
    snprintf(temp_archive, sizeof(temp_archive), "/tmp/clone_archive_%d.tar.gz", getpid());
    
    if (http_download_safe(archive_url, temp_archive, opts->verbose) != 0) {
        if (strcmp(branch, "main") == 0) {
            strcpy(branch, "master");
            switch (repo_type) {
                case REPO_GITHUB:
                    snprintf(archive_url, sizeof(archive_url), 
                            "https://github.com/%s/archive/refs/heads/%s.tar.gz", 
                            opts->url + strlen("https://github.com/"), branch);
                    break;
                default:
                    snprintf(archive_url, sizeof(archive_url), "%s/archive/%s.tar.gz", opts->url, branch);
                    break;
            }
            if (http_download_safe(archive_url, temp_archive, opts->verbose) != 0) {
                log_message(opts->verbose, "Failed to download archive from fallback branch\n");
                unlink(temp_archive);
                return -1;
            }
        } else {
            unlink(temp_archive);
            return -1;
        }
    }
    
    if (extract_archive(temp_archive, opts->path, opts->verbose) != 0) {
        log_message(opts->verbose, "Failed to extract archive\n");
        unlink(temp_archive);
        return -1;
    }
    
    unlink(temp_archive);
    return 0;
}

int parse_clone_options(int argc, char *argv[], CloneOptions *opts) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [URL] [DESTINATION] [-b BRANCH] [-v]\n", argv[0]);
        return -1;
    }
    
    memset(opts, 0, sizeof(CloneOptions));
    strcpy(opts->branch, "main");
    
    int url_found = 0;
    int path_found = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            strncpy(opts->branch, argv[++i], sizeof(opts->branch) - 1);
        } else if (!url_found) {
            strncpy(opts->url, argv[i], sizeof(opts->url) - 1);
            url_found = 1;
        } else if (!path_found) {
            strncpy(opts->path, argv[i], sizeof(opts->path) - 1);
            path_found = 1;
        }
    }
    
    if (!url_found) {
        fprintf(stderr, "Error: URL is required\n");
        return -1;
    }
    
    if (!path_found) {
        char repo_name[256];
        get_repo_name(opts->url, repo_name, sizeof(repo_name));
        strncpy(opts->path, repo_name, sizeof(opts->path) - 1);
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    CloneOptions opts = {0};
    
    if (parse_clone_options(argc, argv, &opts) != 0) {
        return 1;
    }
    
    log_message(opts.verbose, "Cloning into '%s'...\n", opts.path);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (clone_via_archive(&opts) != 0) {
        fprintf(stderr, "Clone failed\n");
        curl_global_cleanup();
        return 1;
    }
    
    curl_global_cleanup();
    log_message(opts.verbose, "Clone completed successfully\n");
    return 0;
}
