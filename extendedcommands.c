#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <reboot/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>


int signature_check_enabled = 1;
int script_assert_enabled = 1;
int ignore_data_media = 1;
int force_use_data_media = 0;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";

void toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("Script Asserts: %s\n", script_assert_enabled ? "Enabled" : "Disabled");
}

void toggle_ignore_data_media()
{
    ignore_data_media = !ignore_data_media;
    ui_print("Backup and Restore /data/media: %s\n", !ignore_data_media ? "Enabled" : "Disabled");
}

void toggle_force_use_data_media()
{
    if (!force_use_data_media)
    {
        if (ensure_path_unmounted("/sdcard") == 0)
        {
            force_use_data_media = 1;
            setup_data_media();
        }
        else
        {
            ui_print("Failed to unmount /sdcard!\n");
            return;
        }
    }
    else
    {
        force_use_data_media = 0;
        unset_data_media();
    }
    ui_print("Use internal storage as /sdcard: %s\n", force_use_data_media ? "Enabled" : "Disabled");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- Installing: %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("Installation aborted.\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    ui_print("\nInstall complete.\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "Choose zip file",
                                "Toggle signature verification",
                                "Toggle script asserts",
                                NULL };
#define ITEM_CHOOSE_ZIP       0
#define ITEM_SIG_CHECK        1
#define ITEM_ASSERTS          2

void show_install_update_menu()
{
    static char* headers[] = {  "Apply update from zip file",
                                "",
                                NULL
    };
    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0, 0);
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu();
                break;
            default:
                return;
        }

    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Failed to open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("No files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu()
{
    if (ensure_path_mounted("/sdcard") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose a zip file to apply",
                                "",
                                NULL
    };

    char* file = choose_file_menu("/sdcard/", ".zip", headers);
    if (file == NULL)
        return;
    static char* confirm_install  = "Are you sure you want to install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "Yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(file);
}

void show_nandroid_restore_menu()
{
    if (ensure_path_mounted("/sdcard") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose an image to restore",
                                "",
                                NULL
    };

    char* file = choose_file_menu("/sdcard/clockworkmod/backup/", NULL, headers);
    if (file == NULL)
        return;

    if (ignore_data_media ? confirm_selection("Are you sure you want to restore?", "Yes - Restore") : confirm_selection("Restore will also affect internal storage. Continue?", "Yes - Restore"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);
}

#ifndef BOARD_UMS_LUNFILE
#define BOARD_UMS_LUNFILE    "/sys/devices/platform/usb_mass_storage/lun0/file"
#endif

void show_mount_usb_storage_menu()
{
    int fd;
    Volume *vol = volume_for_path("/sdcard");
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    if ((write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("Unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
    static char* headers[] = {  "USB Mass Storage mode",
                                "Leaving this menu will unmount",
                                "your SD card from your PC.",
                                "",
                                NULL
    };

    static char* list[] = { "Unmount", NULL };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    char ch = 0;
    if (write(fd, &ch, 1) < 0) {
        LOGE("Unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char level[4];
    FILE* battery = fopen("/sys/class/power_supply/battery/capacity","r");
    fgets(level, 4, battery);
    fclose(battery);

    char* battmsg;
    char* battmsg1 = (atoi(level) < 15 ? "Your battery level is very low (" : "(Current battery level: ");
    char* battmsg2 = "%)";
    asprintf(&battmsg, "%s%s%s", battmsg1, level, battmsg2);

    char* confirm_headers[]  = {  title, battmsg, "  THIS CANNOT BE UNDONE.", "", NULL };
    char* items[] = { "No",
                      confirm,
                      NULL };

    int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
    return chosen_item == 1;
}

int confirm_simple(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "", NULL };
    char* items[] = { "No",
                      confirm,
                      NULL };

    int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
    return chosen_item == 1;
}


#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("Formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("No app2sd partition found. Skipping format of /sd-ext.\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("Formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("Formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("Error mounting %s!\n", path);
        ui_print("Skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_partition_menu()
{
    static char* headers[] = {  "Mounts and Storage Menu",
                                "",
                                NULL
    };

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
        return;

        mountable_volumes = 0;
        formatable_volumes = 0;

        mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
        format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

        for (i = 0; i < num_volumes; ++i) {
            Volume* v = &device_volumes[i];
            if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0)
            {
                sprintf(&mount_menue[mountable_volumes].mount, "Mount %s", v->mount_point);
                sprintf(&mount_menue[mountable_volumes].unmount, "Unmount %s", v->mount_point);
                mount_menue[mountable_volumes].v = &device_volumes[i];
                ++mountable_volumes;
                if (is_safe_to_format(v->mount_point)) {
                    sprintf(&format_menue[formatable_volumes].txt, "Format %s", v->mount_point);
                    format_menue[formatable_volumes].v = &device_volumes[i];
                    ++formatable_volumes;
                }
            }
            else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
            {
                sprintf(&format_menue[formatable_volumes].txt, "Format %s", v->mount_point);
                format_menue[formatable_volumes].v = &device_volumes[i];
                ++formatable_volumes;
            }
        }


    static char* confirm_format  = "Are you sure you want to format";
    static char* confirm = "Yes - Format";
    char confirm_string[255];

    for (;;)
    {

        for (i = 0; i < mountable_volumes; i++)
        {
            MountMenuEntry* e = &mount_menue[i];
            Volume* v = e->v;
            if(is_path_mounted(v->mount_point))
                options[i] = e->unmount;
            else
                options[i] = e->mount;
        }

        for (i = 0; i < formatable_volumes; i++)
        {
            FormatMenuEntry* e = &format_menue[i];

            options[mountable_volumes+i] = e->txt;
        }

        options[mountable_volumes+formatable_volumes] = "Mount USB drive on /sdcard";
        options[mountable_volumes+formatable_volumes + 1] = "Toggle internal storage as /sdcard";
        options[mountable_volumes+formatable_volumes + 2] = "Mount USB storage (USB Mass Storage mode)";
        options[mountable_volumes+formatable_volumes + 3] = NULL;

        int chosen_item = get_menu_selection(headers, &options, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == (mountable_volumes+formatable_volumes))
        {
            if (0 == ensure_path_unmounted("/sdcard"))
            {
                if (0 == try_mount("/dev/block/sda1", "/sdcard", "vfat", NULL))
                    ui_print("Mounted /dev/block/sda1 on /sdcard.\n");
                else
                    ui_print("Error mounting /dev/block/sda1 on /sdcard!\n");
            }
            else
                ui_print("Error unmounting /sdcard!\n");
        }
        if (chosen_item == (mountable_volumes+formatable_volumes+1))
        {
            toggle_force_use_data_media();
        }
        else if (chosen_item == (mountable_volumes+formatable_volumes+2))
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < mountable_volumes)
        {
            MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("Error unmounting %s!\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("Error mounting %s!\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s %s?", confirm_format, v->mount_point);

            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("Formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("Error formatting %s!\n", v->mount_point);
            else
                ui_print("Done.\n");
        }
    }

    free(mount_menue);
    free(format_menue);

}

#define EXTENDEDCOMMAND_SCRIPT "/cache/recovery/extendedcommand"

int extendedcommand_file_exists()
{
    struct stat file_info;
    return 0 == stat(EXTENDEDCOMMAND_SCRIPT, &file_info);
}

int edify_main(int argc, char** argv) {
    load_volume_table();
    process_volumes();
    RegisterBuiltins();
    RegisterRecoveryHooks();
    FinishRegistration();

    if (argc != 2) {
        printf("edify <filename>\n");
        return 1;
    }

    FILE* f = fopen(argv[1], "r");
    if (f == NULL) {
        printf("%s: %s: No such file or directory\n", argv[0], argv[1]);
        return 1;
    }
    char buffer[8192];
    int size = fread(buffer, 1, 8191, f);
    fclose(f);
    buffer[size] = '\0';

    Expr* root;
    int error_count = 0;
    yy_scan_bytes(buffer, size);
    int error = yyparse(&root, &error_count);
    printf("parse returned %d; %d errors encountered\n", error, error_count);
    if (error == 0 || error_count > 0) {

        //ExprDump(0, root, buffer);

        State state;
        state.cookie = NULL;
        state.script = buffer;
        state.errmsg = NULL;

        char* result = Evaluate(&state, root);
        if (result == NULL) {
            printf("result was NULL, message is: %s\n",
                   (state.errmsg == NULL ? "(NULL)" : state.errmsg));
            free(state.errmsg);
        } else {
            printf("result is [%s]\n", result);
        }
    }
    return 0;
}

int run_script(char* filename)
{
    struct stat file_info;
    if (0 != stat(filename, &file_info)) {
        printf("Error executing stat on file: %s\n", filename);
        return 1;
    }

    int script_len = file_info.st_size;
    char* script_data = (char*)malloc(script_len + 1);
    FILE *file = fopen(filename, "rb");
    fread(script_data, script_len, 1, file);
    // supposedly not necessary, but let's be safe.
    script_data[script_len] = '\0';
    fclose(file);
    LOGI("Running script:\n");
    LOGI("\n%s\n", script_data);

    int ret = run_script_from_buffer(script_data, script_len, filename);
    free(script_data);
    return ret;
}

int run_and_remove_extendedcommand()
{
    char tmp[PATH_MAX];
    sprintf(tmp, "cp %s /tmp/%s", EXTENDEDCOMMAND_SCRIPT, basename(EXTENDEDCOMMAND_SCRIPT));
    __system(tmp);
    remove(EXTENDEDCOMMAND_SCRIPT);
    int i = 0;
    for (i = 20; i > 0; i--) {
        ui_print("Waiting for SD card to mount (%ds)\n", i);
        if (ensure_path_mounted("/sdcard") == 0) {
            ui_print("SD card mounted...\n");
            break;
        }
        sleep(1);
    }
    remove("/sdcard/clockworkmod/.recoverycheckpoint");
    if (i == 0) {
        ui_print("Timed out waiting for SD card...");
    }

    sprintf(tmp, "/tmp/%s", basename(EXTENDEDCOMMAND_SCRIPT));
    return run_script(tmp);
}

void show_nandroid_advanced_backup_menu(){
    static char* advancedheaders[] = { "Choose the partitions to backup.",
                    NULL
    };

    int backup_list [5];
    char* list[6];

    backup_list[0] = 1;
    backup_list[1] = 1;
    backup_list[2] = 1;
    backup_list[3] = 1;
    backup_list[4] = 1;

  

    list[5] = "Perform Backup";
    list[6] = NULL;

    int cont = 1;
    for (;cont;) {
        if (backup_list[0] == 1)
            list[0] = "Backup boot: Yes";
        else
            list[0] = "Backup boot: No";
    
        if (backup_list[1] == 1)
            list[1] = "Backup recovery: Yes";
        else
            list[1] = "Backup recovery: No";
    
        if (backup_list[2] == 1)
            list[2] = "Backup system: Yes";
        else
            list[2] = "Backup system: No";

        if (backup_list[3] == 1)
            list[3] = "Backup data: Yes";
        else
            list[3] = "Backup data: No";   

        if (backup_list[4] == 1)
            list[4] = "Backup cache: Yes";
        else
            list[4] = "Backup cache: No";   

        int chosen_item = get_menu_selection (advancedheaders, list, 0, 0);
        switch (chosen_item) {
            case GO_BACK: return;
            case 0: backup_list[0] = !backup_list[0];
                break;
            case 1: backup_list[1] = !backup_list[1];
                break;
            case 2: backup_list[2] = !backup_list[2];
                break;
            case 3: backup_list[3] = !backup_list[3];
                break;
            case 4: backup_list[4] = !backup_list[4];
                break;
            case 5: backup_list[5] = !backup_list[5];
                break;

            case 6: cont = 0;
                    break;
        }
    }

    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL){
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
   }else{
    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
   }

   return nandroid_advanced_backup(backup_path, backup_list[0], backup_list[1], backup_list[2], backup_list[3], backup_list[4], backup_list[5], 0);
     
}

void show_nandroid_advanced_restore_menu()
{
    if (ensure_path_mounted("/sdcard") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "Choose an image to restore",
                                "",
                                "Choose an image to restore",
                                "first. The next menu will",
                                "show you more options.",
                                "",
                                NULL
    };

    char* file = choose_file_menu("/sdcard/clockworkmod/backup/", NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "Nandroid Advanced Restore",
                                "",
                                NULL
    };

    static char* list[] = { "Restore boot",
                            "Restore system",
                            "Restore data",
                            "Restore cache",
                            NULL
    };
    
    char tmp[PATH_MAX];
    if (0 != get_partition_device("wimax", tmp)) {
        // disable wimax restore option
        list[5] = NULL;
    }

    static char* confirm_restore  = "Are you sure you want to restore?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "Yes - Restore boot"))
                nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "Yes - Restore system"))
                nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "Yes - Restore data"))
                nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "Yes - Restore cache"))
                nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL
    };

    static char* list[] = { "Backup",
                            "Restore",
                            "Advanced Backup",
                            "Advanced Restore",
                            "Toggle backup and restore of internal storage (/data/media)",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
                if (!is_data_media || confirm_simple("For compatibility it is not recommended to backup to internal storage. Continue?", "Yes - Backup"))
                {
                    char backup_path[PATH_MAX];
                    time_t t = time(NULL);
                    struct tm *tmp = localtime(&t);
                    if (tmp == NULL)
                    {
                        struct timeval tp;
                        gettimeofday(&tp, NULL);
                        sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                    }
                    else
                    {
                        strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                    }
                    nandroid_backup(backup_path);
                }
            }
            break;
        case 1:
            show_nandroid_restore_menu();
            break;
        case 2:
            if (!is_data_media || confirm_simple("For compatibility it is not recommended to backup to internal storage. Continue?", "Yes - Backup"))
                show_nandroid_advanced_backup_menu();
            break;
        case 3:
            show_nandroid_advanced_restore_menu();
            break;
        case 4:
            toggle_ignore_data_media();
            break;
    }
}

void show_advanced_menu()
{
    static char* headers[] = {  "Advanced and Debugging Menu",
                                "",
                                NULL
    };

    static char* list[] = { "Reboot recovery",
                            "Report error",
                            "Key test",
                            "Show log",
#ifndef BOARD_HAS_SMALL_RECOVERY
                            "Partition SD card",
                            "Fix permissions",
#ifdef BOARD_HAS_SDCARD_INTERNAL
                            "Partition internal SD card",
#endif
#endif
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
                reboot_wrapper("recovery");
                break;
            case 1:
                handle_failure(1);
                break;
            case 2:
            {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("Key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 3:
            {
                ui_printlogtail(12);
                break;
            }
            case 4:
            {
                if (is_data_media())
                    ui_print("Internal storage may not be paritioned!\n");
                else if (confirm_selection("All data on the SD card will be wiped. Continue?", "Yes - Partition"))
                {
                    static char* ext_sizes[] = { "0M",
                                                 "128M",
                                                 "256M",
                                                 "512M",
                                                 "1024M",
                                                 "2048M",
                                                 "4096M",
                                                 NULL };

                    static char* swap_sizes[] = { "0M",
                                                  "32M",
                                                  "64M",
                                                  "128M",
                                                  "256M",
                                                  NULL };

                    static char* ext_headers[] = { "Ext Size", "", NULL };
                    static char* swap_headers[] = { "Swap Size", "", NULL };

                    int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                    if (ext_size == GO_BACK)
                        continue;

                    int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
                    if (swap_size == GO_BACK)
                        continue;

                    char sddevice[256];
                    Volume *vol = volume_for_path("/sdcard");
                    strcpy(sddevice, vol->device);
                    // we only want the mmcblk, not the partition
                    sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                    char cmd[PATH_MAX];
                    setenv("SDPATH", sddevice, 1);
                    sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                    ui_print("Partitioning SD card...\n");
                    if (0 == __system(cmd))
                        ui_print("Done!\n");
                    else
                        ui_print("An error occured while partitioning your SD card. Please check the recovery log for more details.\n");
                }
                break;
            }
            case 5:
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("Fixing permissions...\n");
                __system("fix_permissions");
                ui_print("Done!\n");
                break;
            }
            case 6:
            {
                if (confirm_selection("All data on the internal SD card will be wiped. Continue?", "Yes - Partition"))
                {
                    static char* ext_sizes[] = { "0M",
                                                 "128M",
                                                 "256M",
                                                 "512M",
                                                 "1024M",
                                                 "2048M",
                                                 "4096M",
                                                 NULL };

                    static char* swap_sizes[] = { "0M",
                                                  "32M",
                                                  "64M",
                                                  "128M",
                                                  "256M",
                                                  NULL };

                    static char* ext_headers[] = { "Data Size", "", NULL };
                    static char* swap_headers[] = { "Swap Size", "", NULL };

                    int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                    if (ext_size == GO_BACK)
                        continue;

                    int swap_size = 0;
                    if (swap_size == GO_BACK)
                        continue;

                    char sddevice[256];
                    Volume *vol = volume_for_path("/emmc");
                    strcpy(sddevice, vol->device);
                    // we only want the mmcblk, not the partition
                    sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                    char cmd[PATH_MAX];
                    setenv("SDPATH", sddevice, 1);
                    sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                    ui_print("Partitioning internal SD card...\n");
                    if (0 == __system(cmd))
                        ui_print("Done!\n");
                    else
                        ui_print("An error occured while partitioning your internal SD card. Please check the recovery log for more details.\n");
                }
                break;
            }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("Unable to get recovery.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    if (has_datadata()) {
        write_fstab_root("/datadata", file);
    }
    write_fstab_root("/system", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    fclose(file);
    LOGI("Completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("Checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("Unable process volume! Skipping...\n");
        return 0;
    }
    
    ui_print("%s may be rfs. Checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("Checking for ext4 partitions...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("Done!\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%s", backup_name);

    ui_set_show_text(1);
    ui_print("Filesystems need to be converted to ext4.\n");
    ui_print("A backup and restore will now take place.\n");
    ui_print("If anything goes wrong, your backup will be\n");
    ui_print("named %s. Try restoring it\n", backup_name);
    ui_print("in case of error.\n");

    nandroid_backup(backup_path);
    nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp     mp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("    mp/recovery.log was copied to /sdcard/clockworkmod/recovery.log.\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}
