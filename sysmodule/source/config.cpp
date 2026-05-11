#include "config.h"

/* --------------------------------------------------------------------------
 * Tiny INI parser
 * Handles:
 *   [section]
 *   key = value   (leading/trailing space stripped)
 *   ; comment or # comment
 * -------------------------------------------------------------------------- */

static void trim(char *s)
{
    /* leading */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' '  || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

static void set_defaults(nx_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* ip is intentionally left empty — auto_ip fills it in */
    strncpy(cfg->subnet_net,  SUBNET_NET,  NX_IP_MAX - 1);
    strncpy(cfg->subnet_mask, SUBNET_MASK, NX_IP_MAX - 1);
}

int nx_config_load(nx_config_t *cfg)
{
    set_defaults(cfg);

    FILE *f = fopen(NX_CONFIG_PATH, "r");
    if (!f) {
        LLOG(LLOG_WARNING, "config: cannot open %s", NX_CONFIG_PATH);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line);

        /* skip comments and section headers */
        if (line[0] == '\0' || line[0] == ';' ||
            line[0] == '#'  || line[0] == '[')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if      (strcmp(key, "relay_addr")   == 0)
            strncpy(cfg->relay_addr,  val, NX_STR_MAX - 1);
        else if (strcmp(key, "ip")           == 0)
            strncpy(cfg->my_ip,       val, NX_IP_MAX  - 1);
        else if (strcmp(key, "subnet_net")   == 0)
            strncpy(cfg->subnet_net,  val, NX_IP_MAX  - 1);
        else if (strcmp(key, "subnet_mask")  == 0)
            strncpy(cfg->subnet_mask, val, NX_IP_MAX  - 1);
        else if (strcmp(key, "username")     == 0)
            strncpy(cfg->username,    val, NX_STR_MAX - 1);
        else if (strcmp(key, "password")     == 0)
            strncpy(cfg->password,    val, NX_STR_MAX - 1);
    }

    fclose(f);

    if (cfg->relay_addr[0] == '\0') {
        LLOG(LLOG_WARNING, "config: relay_addr not set in %s", NX_CONFIG_PATH);
        return -1;
    }

    LLOG(LLOG_INFO, "config: relay=%s ip=%s",
         cfg->relay_addr, cfg->my_ip[0] ? cfg->my_ip : "(auto)");
    return 0;
}

/* --------------------------------------------------------------------------
 * Auto-IP: deterministic unique IP in 10.13.1.1 – 10.13.254.254
 * derived from the device serial number via FNV-1a hash.
 * Avoids the virtual gateway address 10.13.37.1 and the .0 / .255
 * boundary addresses.
 * -------------------------------------------------------------------------- */
void nx_config_auto_ip(nx_config_t *cfg)
{
    if (cfg->my_ip[0] != '\0') return; /* already set in config file */

    /* FNV-1a hash of the device serial number */
    SetSysSerialNumber serial;
    uint32_t h = 0x811c9dc5u;
    if (R_SUCCEEDED(setsysGetSerialNumber(&serial))) {
        for (size_t i = 0; i < sizeof(serial.number) && serial.number[i]; i++) {
            h ^= (uint8_t)serial.number[i];
            h *= 0x01000193u;
        }
    } else {
        /* Fallback: use a second hash from a different seed */
        h = 0x5851f42du;
    }

    /* Map to 10.13.X.Y
     * X in [1, 254], Y in [1, 254], excluding 37.1 (virtual gateway) */
    uint8_t x = (h >> 8) & 0xFF;
    uint8_t y =  h        & 0xFF;

    if (x == 0)   x = 1;
    if (x == 255) x = 254;
    if (y == 0)   y = 1;
    if (y == 255) y = 254;

    /* Don't collide with the virtual gateway 10.13.37.1 */
    if (x == 37 && y == 1) y = 2;

    snprintf(cfg->my_ip, NX_IP_MAX, "10.13.%u.%u", (unsigned)x, (unsigned)y);
    LLOG(LLOG_INFO, "config: auto-assigned IP %s", cfg->my_ip);
}

/* --------------------------------------------------------------------------
 * Write a minimal config template — only relay_addr is required from the
 * user.  Everything else is handled automatically.
 * -------------------------------------------------------------------------- */
int nx_config_write_default(void)
{
    /* Create directory if needed */
    mkdir(NX_CONFIG_DIR, 0777);

    FILE *f = fopen(NX_CONFIG_PATH, "w");
    if (!f) {
        LLOG(LLOG_ERROR, "config: cannot create %s", NX_CONFIG_PATH);
        return -1;
    }

    fprintf(f,
        "; switch-lan-play configuration\n"
        "; Use the LanPlay Setup homebrew app to configure this file,\n"
        "; or edit it manually and reboot.\n"
        "\n"
        "[server]\n"
        "; Address of the relay server (host:port or just host for default port)\n"
        "relay_addr = YOUR_SERVER:11451\n"
        "\n"
        "; Optional: only needed if the server requires a password\n"
        "; username =\n"
        "; password =\n"
        "\n"
        "; Advanced: manually pin the virtual IP (leave commented for auto)\n"
        "; ip = 10.13.1.1\n"
    );

    fclose(f);
    LLOG(LLOG_INFO, "config: default config written to %s", NX_CONFIG_PATH);
    return 0;
}

/* --------------------------------------------------------------------------
 * nx_config_save_relay — atomically replace only the relay_addr line.
 * Used by the homebrew configurator NRO so it doesn't clobber auth settings.
 * -------------------------------------------------------------------------- */
#define MAX_CONFIG_LINES 64
#define MAX_LINE_LEN     512

int nx_config_save_relay(const char *relay_addr)
{
    /* Create directory */
    mkdir(NX_CONFIG_DIR, 0777);

    /* Read current file (if any) into memory */
    char lines[MAX_CONFIG_LINES][MAX_LINE_LEN];
    int  nlines = 0;
    bool found  = false;

    FILE *r = fopen(NX_CONFIG_PATH, "r");
    if (r) {
        while (nlines < MAX_CONFIG_LINES - 1 &&
               fgets(lines[nlines], MAX_LINE_LEN, r))
            nlines++;
        fclose(r);
    }

    /* Locate an existing relay_addr line and update it */
    for (int i = 0; i < nlines; i++) {
        char tmp[MAX_LINE_LEN];
        strncpy(tmp, lines[i], MAX_LINE_LEN - 1);
        tmp[MAX_LINE_LEN - 1] = '\0';
        trim(tmp);
        if (strncmp(tmp, "relay_addr", 10) == 0 && strchr(tmp, '=')) {
            snprintf(lines[i], MAX_LINE_LEN, "relay_addr = %s\n", relay_addr);
            found = true;
            break;
        }
    }

    /* Not found: append (or write fresh) */
    if (!found) {
        if (nlines == 0) {
            /* Fresh file */
            snprintf(lines[nlines++], MAX_LINE_LEN, "; switch-lan-play configuration\n");
            snprintf(lines[nlines++], MAX_LINE_LEN, "[server]\n");
        }
        snprintf(lines[nlines++], MAX_LINE_LEN, "relay_addr = %s\n", relay_addr);
    }

    FILE *w = fopen(NX_CONFIG_PATH, "w");
    if (!w) {
        LLOG(LLOG_ERROR, "config: cannot write %s", NX_CONFIG_PATH);
        return -1;
    }
    for (int i = 0; i < nlines; i++)
        fputs(lines[i], w);
    fclose(w);

    LLOG(LLOG_INFO, "config: relay_addr saved as '%s'", relay_addr);
    return 0;
}
