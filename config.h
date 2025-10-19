#ifndef CONFIG_H
#define CONFIG_H

// service configuration
#define GITHUB_URL "https://github.com"
#define GITHUB_API "https://api.github.com/repos"
#define GITLAB_URL "https://gitlab.com" 
#define GITLAB_API "https://gitlab.com/api/v4/projects"
#define CODEBERG_URL "https://codeberg.org"
#define CODEBERG_API "https://codeberg.org/api/v1/repos"

// archive preferences
#define DEFAULT_BRANCH "main"
#define FALLBACK_FORMAT "zip"
#define CODEBERG_PREFER_TAR_GZ 0  // set to 1 to prefer tar.gz for codeberg

// network settings
#define USER_AGENT "tinygit/1.0"
#define TIMEOUT_SECONDS 30

#endif
