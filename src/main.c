#include <ahci.h>
#include <ata.h>
#include <block.h>
#include <chifs.h>
#include <console.h>
#include <interrupts.h>
#include <keyboard.h>
#include <limine.h>
#include <memory.h>
#include <nvme.h>
#include <pci.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ufs.h>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static int is_interp = 0;

static size_t unescape_newlines(char *text) {
    char *read = text;
    char *write = text;

    while (*read != '\0') {
        if (read[0] == '\\' && (read[1] == 'n' || read[1] == 'N')) {
            *write++ = '\n';
            read += 2;
        } else {
            *write++ = *read++;
        }
    }

    *write = '\0';
    return (size_t)(write - text);
}

static void hcf(void) {
    for (;;) {
        asm ("hlt");
    }
}

__attribute__((noreturn))
static void panic(void) {
    printf("\Kettenkrad panic!\n");
    asm volatile ("ud2");
    __builtin_unreachable();
}

static void heading(const char *title) {
    console_set_color(CONSOLE_CYAN, CONSOLE_BLACK);
    printf("[%s]\n", title);
    console_set_color(CONSOLE_WHITE, CONSOLE_BLACK);
}

static void chito_ascii(void) {

    console_set_color(CONSOLE_GREEN, CONSOLE_BLACK);

	printf("                                                                                                    \n");
	printf("                                                                                                    \n");
	printf("                                                                                                    \n");
	printf("                                                                                                    \n");
	printf("                                           @@@@@@@@%%%%%%@@@@@@@@@                                     \n");
	printf("                                    @@%%#************************%%@@@                                \n");
	printf("                                 @@#*********************************%%@@@                           \n");
	printf("                              @@#****************************************%%@@                        \n");
	printf("                            @%%**********************************************%%@                      \n");
	printf("                          @@**************************************************%%@                    \n");
	printf("                        @@#*****************************************************%%@                  \n");
	printf("                       @%%*********************************************************@@                \n");
	printf("                      @#***********************************************************%%@               \n");
	printf("                    @@#*************************************************************%%@              \n");
	printf("                  @@@####%%%%##********************************************************#@             \n");
	printf("        @@@@@@@%%%%#********************************************************************#@            \n");
	printf(" @@%%%%##********************************************************************************%%@           \n");
	printf("@%%++**##%%%%%%%%%%%%##**##********************************************************************@           \n");
	printf(" @@@%%+===========*%%####%%%%@%%%%%%%%##********************************************************@@          \n");
	printf("      @@@%%%%#++==+%%#######@#######%%*=+*#%%##************************************##********%%@          \n");
	printf("               @@######%%@###%%###%%.    -%%%%+   .-#%%@%%%%#****************************%%%%*****%%@          \n");
	printf("               @######@@#%%+.##%%*      =%%%%.          =%%*.:-*@%%%%#*********************#%%#*%%@@         \n");
	printf("              @%%####%%%%@%%- .-%%%%:       +@:           *%%.    ##%%:%%###%%%%@%%@#*****************%%@        \n");
	printf("             @%%####%%=@@%%###%%%%#%%%%@#+.  +#           .@-     *%%-  *###%%-.%%####%%@%%************#@@      \n");
	printf("            @%%####%%@%%###########%%=  :*%%:           .=.=*###%%@#*+:*%%#%%  ###%%######%%%%%%#********%%@     \n");
	printf("           @@######:%%###########%%-    --            *@%%####%%%%##%%%%:+@#. =%%@######%%%%####@@%%#*****@@   \n");
	printf("          @@#%%%%##%%-  :#%%%%######%%=   :*+            .%%###########%%*    *#@%%######@#####%%*===+#%%#*#@@ \n");
	printf("          @%%@@%%##%%.       ..:--=+=-                 -%%%%#########%%*     :@######%%%%#####%%@%%%%**+==*%%##@\n");
	printf("         @%%@ @###%%                                     :*%%%%######     .*%%######@%%#####%%@    @@@@%%%%@#\n");
	printf("        @@@ @%%###%%                                          ....-=+*+:.%%######%%. #####@             \n");
	printf("       @@@  @####%%.                                                   =%%######*  +%%##%%@             \n");
	printf("       @@  @@####%%+                                                  .%%######%%.  +%%##@@             \n");
	printf("      @@   @%%#####%%:                                                 *######%%+   ###%%@              \n");
	printf("     @@    @######@%% .  .                                           -%%######%%. =%%##%%@               \n");
	printf("           @#####%%@ %%. ..                    :==.                  .%%######%%%%#%%###%%@                \n");
	printf("          @%%@%%###@   @#:.   .                                      *#######@####%%@@                 \n");
	printf("          @@@%%##%%@     @@*...                                     =%%######%%%%##%%@@                   \n");
	printf("          @@@%%##@         @@#-.......       .      ..            .%%#######@#%%@@                     \n");
	printf("         @@@ @#%%@              @@#=::  :*%%#**%%.   ***#%%%%#+:   ..-#%%######@%%@                        \n");
	printf("         @@  @#%%@               @%%*#%%#@#*****##=+%%%%*********##%%+#%%######@#*#@                       \n");
	printf("             @%%@             @%%%%%%*+@##%%%%*******  +#**********%%@@@#####@@%%+++%%@                      \n");
	printf("              @@            @*+++*%%%%##@*%%#****#*%%%%**********#%%#%%###%%@#%%@#%%%%%%@@                      \n");
	printf("                           @#++++*@###@*+*@#***##*******#%%%%*++*%%%%@%%#%%#@*+++++%%@                     \n");
	printf("                          @@+++++%%####%%+#%%*#%%*#%%%%#**#%%%%*+*#%%#*@@%%###%%#*++++++*@                     \n");
	printf("                          @#++++#%%###%%#++++#%%#%%**#%%%%#%%%%#*++++*#%%####%%*+++++++*@                     \n");
	printf("                          @*+++*%%####%%#+++++++#%%%%*++++++++++++%%#####%%+++****#%%@                     \n");
	printf("                           @%%##%%#####%%#*****##%%%%%%****++++++++#%%#####%%%%%%##**%%@                       \n");
	printf("                          @#++*%%#####%%++++++++++++++++++++++*%%#####%%*+++++++@@                      \n");
	printf("                         @%%+++%%######%%++++++++++++++++++++++#%%#####%%++++++++*@                      \n");
	printf("                         @+++*%%#######++++++++++++++++++++++%%######%%+++++++++%%@                     \n");
	printf("                         %%%%  %%%%#%%%%  @@         %%%%%%         @%%####%%%%@                                \n");
	printf("                                                                                                    \n");
	printf("                                                                                                    \n");
	printf("                                                                                                    \n");
	printf("                                                                                                    \n");

}

static void ksetup(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];

    if (!console_init(framebuffer, 1)) {
        hcf();
    }

    bool real_heap = memory_init();

    bool irqs = interrupts_init();
    bool keyboard = keyboard_init();

    interrupts_enable();

    ata_init();
    ahci_init();
    nvme_init();
    ufs_init();

    uint32_t tsc_lo, tsc_hi;
    __asm__ volatile ("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    srand(tsc_lo ^ tsc_hi);

    console_set_color(CONSOLE_BRIGHT, CONSOLE_BLACK);
}

static void list_storage_controllers(void) {
    static const char *subclass_name[] = {
        "SCSI", "IDE", "floppy", "IPI", "RAID", "ATA", "SATA", "SAS", "NVM", "UFS"
    };

    struct pci_device dev, previous;
    struct pci_device *cursor = NULL;
    int storage = 0, total = 0;

    while (pci_next(cursor, &dev)) {
        previous = dev;
        cursor = &previous;
        total++;

        if (dev.class_code != 0x01) {
            continue;
        }

        const char *name = dev.subclass < sizeof subclass_name / sizeof subclass_name[0]
                         ? subclass_name[dev.subclass] : "other";

        storage++;
        printf("    %02x:%02x.%u %04x:%04x class %02x.%02x.%02x (%s)\n",
               dev.bus, dev.slot, dev.func, dev.vendor_id, dev.device_id,
               dev.class_code, dev.subclass, dev.prog_if, name);
    }

    printf("    %d storage controller%s of %d PCI functions\n",
           storage, storage == 1 ? "" : "s", total);
}

static void list_disks(void) {
    heading("Disks");

    static const char *bus_name[ATA_BUS_COUNT] = { "primary", "secondary" };
    static const char *drive_name[ATA_DRIVE_COUNT] = { "master", "slave" };

    bool any = false;

    for (int bus = 0; bus < ATA_BUS_COUNT; bus++) {
        for (int drive = 0; drive < ATA_DRIVE_COUNT; drive++) {
            const struct ata_drive *info = ata_drive_info(bus, drive);

            if (info == NULL) {
                continue;
            }

            any = true;
            printf("  %s %s: %s, %u sectors (%u MiB)\n",
                   bus_name[bus], drive_name[drive], info->model,
                   info->sectors, info->sectors / (1024 * 1024 / ATA_SECTOR_SIZE));
        }
    }

    for (int port = 0; port < AHCI_MAX_PORTS; port++) {
        const struct ahci_drive *info = ahci_drive_info(port);

        if (info == NULL) {
            continue;
        }

        any = true;
        printf("  ahci port %d: %s, %llu sectors (%llu MiB)\n",
               port, info->model, (unsigned long long)info->sectors,
               (unsigned long long)(info->sectors / (1024 * 1024 / AHCI_SECTOR_SIZE)));
    }

    const struct nvme_drive *nvme = nvme_drive_info();

    if (nvme != NULL) {
        any = true;
        printf("  nvme: %s, %llu sectors of %u bytes (%llu MiB)\n",
               nvme->model, (unsigned long long)nvme->sectors, nvme->sector_size,
               (unsigned long long)(nvme->sectors / (1024 * 1024 / nvme->sector_size)));
    }

    for (int lun = 0; lun < UFS_MAX_LUNS; lun++) {
        const struct ufs_lun *unit = ufs_lun_info(lun);

        if (unit == NULL) {
            continue;
        }

        any = true;
        printf("  ufs lun %d: %s, %llu sectors of %u bytes (%llu MiB)\n",
               lun, unit->model, (unsigned long long)unit->sectors, unit->sector_size,
               (unsigned long long)(unit->sectors / (1024 * 1024 / unit->sector_size)));
    }

    if (!any) {
        printf("  none found\n");
        printf("    ahci: %s\n", ahci_status());
        printf("    nvme: %s\n", nvme_status());
        printf("    ufs:  %s\n", ufs_status());
        list_storage_controllers();
    }
}

static void offer_format(void) {
    printf("\n  Format a device with ChiFS? This ERASES everything on it.\n");

    for (int i = 0; i < block_device_count(); i++) {
        const struct block_device *dev = block_device_get(i);

        printf("    %-6s %s, %llu MiB\n", dev->name, dev->model,
               (unsigned long long)(block_capacity(dev) / (1024 * 1024)));
    }

    char line[32];

    printf("  Device to format (Enter to skip): ");

    if (gets_s(line, sizeof line) == NULL) {
        return;
    }

    const struct block_device *dev = block_device_by_name(line);

    if (dev == NULL) {
        printf("  Skipped.\n");
        return;
    }

    console_set_color(CONSOLE_RED, CONSOLE_BLACK);
    printf("  Everything on %s will be lost maybe forever. Type ERASE to confirm: ", dev->name);
    console_set_color(CONSOLE_WHITE, CONSOLE_BLACK);

    if (gets_s(line, sizeof line) == NULL || strcmp(line, "ERASE") != 0) {
        printf("  Skipped.\n");
        return;
    }

    if (!chifs_format(dev, "ChiOS")) {
        printf("  Format failed.\n");
        return;
    }

    if (!chifs_mount(dev)) {
        printf("  Formatted! but would not mount. :(\n");
        return;
    }

    chifs_create("JOURNAL");

    printf("  Formatted and mounted %s.\n", dev->name);
}

static void show_files(void) {
    struct chifs_dirent entries[32];
    int count = chifs_list(entries, 32);

    if (count <= 0) {
        printf("  (empty)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        printf("    %-32s %llu bytes, %u block%s\n", entries[i].name,
               (unsigned long long)entries[i].size, entries[i].blocks,
               entries[i].blocks == 1 ? "" : "s");
    }
}

static void list_memory_map(void) {
    heading("Memory map");

    struct limine_memmap_response *memmap = memory_map();

    if (memmap == NULL) {
        printf("  no memory map available\n");
        return;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        printf("  %#018llx-%#018llx %8llu KiB  %s\n",
               (unsigned long long)entry->base,
               (unsigned long long)(entry->base + entry->length),
               (unsigned long long)(entry->length / 1024),
               memmap_type_name(entry->type));
    }
}

static void mount_filesystem(void) {
    heading("Filesystem");

    block_scan();

    if (block_device_count() == 0) {
        printf("  no block devices to look at\n");
        return;
    }

    for (int i = 0; i < block_device_count(); i++) {
        if (!chifs_mount(block_device_get(i))) {
            continue;
        }

        struct chifs_info info;

        chifs_stat(&info);
        printf("  ChiFS \"%s\" on %s: %llu of %llu blocks free, %u of %u files used\n",
               info.label, info.device,
               (unsigned long long)info.free_blocks,
               (unsigned long long)info.total_blocks,
               info.used_inodes, info.total_inodes);
        show_files();

        return;
    }

    printf("  no ChiFS volume on any of the %d block device%s\n",
           block_device_count(), block_device_count() == 1 ? "" : "s");

    offer_format();
}

static void execute_command(char *user_cmd) {
    char *space = strchr(user_cmd, ' ');

    if (space != NULL) {
        int cmd_length = space - user_cmd;

        strncpy(user_cmd, user_cmd, cmd_length);
        user_cmd[cmd_length] = '\0';
    } else {
        strcpy(user_cmd, user_cmd);
    }

    if (strcmp(user_cmd, "HELP") == 0) {
        printf("\n====================================\n");
        printf("................HELP................\n");
        printf("====================================\n");
        printf("| %-33s|\n", "HELP      -> You are here.");
        printf("| %-33s|\n", "TRUTH     -> Random quote.");
        printf("| %-33s|\n", "ECHO      -> Display text.");
        printf("| %-33s|\n", "LS        -> List files.");
        printf("| %-33s|\n", "CHITO     -> Read file.");
        printf("| %-33s|\n", "JOURNAL   -> Write in journal.");
        printf("| %-33s|\n", "FILE      -> Create file.");
        printf("| %-33s|\n", "NUKO      -> Delete file.");
        printf("| %-33s|\n", "WRITE     -> Overwrite file.");
        printf("| %-33s|\n", "APPEND    -> Append to file.");
        printf("| %-33s|\n", "FORMAT    -> Format disk.");
        printf("| %-33s|\n", "LSD       -> List disks.");
        printf("| %-33s|\n", "LSSC      -> List controllers.");
        printf("| %-33s|\n", "KANAZAWA  -> List memory map.");
        printf("| %-33s|\n", "YUURI     -> Uh oh.");
        printf("| %-33s|\n", "ISHII     -> Calculate.");
        printf("| %-33s|\n", "ASCII     -> Ascii art.");
        printf("| %-33s|\n", "RUN       -> Run command file.");
        printf("!----------------------------------!\n");
        printf("| %-33s|\n", "Usage:");
        printf("| %-33s|\n", "HELP");
        printf("| %-33s|\n", "TRUTH");
        printf("| %-33s|\n", "ECHO <text>");
        printf("| %-33s|\n", "LS");
        printf("| %-33s|\n", "CHITO <file>");
        printf("| %-33s|\n", "JOURNAL <text>");
        printf("| %-33s|\n", "FILE <name>");
        printf("| %-33s|\n", "NUKO <file>");
        printf("| %-33s|\n", "WRITE <file> <text>");
        printf("| %-33s|\n", "APPEND <file> <text>");
        printf("| %-33s|\n", "FORMAT");
        printf("| %-33s|\n", "LSD");
        printf("| %-33s|\n", "LSSC");
        printf("| %-33s|\n", "KANAZAWA");
        printf("| %-33s|\n", "YUURI");
        printf("| %-33s|\n", "ISHII <int> <operator> <int>");
        printf("| %-33s|\n", "ASCII");
        printf("| %-33s|\n", "RUN <file>");
        printf("!__________________________________!\n");
    }
    else if (strcmp(user_cmd, "TRUTH") == 0) {
        int min = 1;
        int max = 10;
        int range = max - min + 1;

        int random_num = (rand() % range) + min;
        if (random_num == 1) {printf("\nI never knew nighttime was this bright.\n");}
        if (random_num == 2) {printf("\nIf they'd preserved food instead of making weapons, our lives would be so much easier.\n");}
        if (random_num == 3) {printf("\nI never knew nighttime was this bright.\n");}
        if (random_num == 4) {printf("\nSays it's chocolate flavored. No idea what chocolate is, though...\n");}
        if (random_num == 5) {printf("\nIt's so blue, Chi. It's like we're in the sky.\n");}
        if (random_num == 6) {printf("\nHey, I wonder what it feels like to live in the water.\n");}
        if (random_num == 7) {printf("\nIs the afterlife like this? No warmth, so dark you can't see, with nobody there...\n");}
        if (random_num == 8) {printf("\nIn the end, we're back to a cycle of resupplying and traveling. That means the road we travel is our house.\n");}
        if (random_num == 9) {printf("\nMemories fade, so we write them down.\n");}
        if (random_num == 10) {printf("\nMaybe this is what they call 'music'. I heard it's when high and low sounds have a rhythm and connect together.\n");}
    }
    else if (strcmp(user_cmd, "ECHO") == 0) {
        char *argument = space + 1;
        printf("\n%s\n", argument);
    }
    else if (strcmp(user_cmd, "LS") == 0) {
    	show_files();
    }
    else if (strcmp(user_cmd, "CHITO") == 0) {
        char *argument = space + 1;
        int64_t size = chifs_size(argument);
        char *buf = malloc((size_t)size + 1);
        int64_t got = chifs_read_file(argument, buf, (size_t)size);
        buf[got] = '\0';
        printf("\n%s\n", buf);
        free(buf);
    }
    else if (strcmp(user_cmd, "JOURNAL") == 0) {
        char *argument = space + 1;
        unescape_newlines(argument);
        char entry[256];
        int len = snprintf(entry, sizeof entry, "%s\n", argument);
        int64_t offset = chifs_size("JOURNAL");

        chifs_write("JOURNAL", (uint64_t)offset, entry, (size_t)len);
        printf("\nScribble Scribble Scribble... Journal entry written!\n");
    }
    else if (strcmp(user_cmd, "FILE") == 0) {
        char *argument = space + 1;

        chifs_create(argument);
    }
    else if (strcmp(user_cmd, "NUKO") == 0) {
        char *argument = space + 1;

        chifs_remove(argument);
    }
    else if (strcmp(user_cmd, "WRITE") == 0) {
        char *argument = space + 1;
        char *text = strchr(argument, ' ');

        *text = '\0';
        text++;

        size_t text_len = unescape_newlines(text);
        chifs_write_file(argument, text, text_len);
    }
    else if (strcmp(user_cmd, "APPEND") == 0) {
        char *argument = space + 1;
        char *text = strchr(argument, ' ');

        *text = '\0';
        text++;

        unescape_newlines(text);
        char entry[256];
        int len = snprintf(entry, sizeof entry, "%s\n", text);
        int64_t offset = chifs_size(argument);

        chifs_write(argument, (uint64_t)offset, entry, (size_t)len);
    }
    else if (strcmp(user_cmd, "FORMAT") == 0) {
    	offer_format();
    }
    else if (strcmp(user_cmd, "LSD") == 0) {
    	list_disks();
    }
    else if (strcmp(user_cmd, "LSSC") == 0) {
    	list_storage_controllers();
    }
    else if (strcmp(user_cmd, "KANAZAWA") == 0) {
    	list_memory_map();
    }
    else if (strcmp(user_cmd, "YUURI") == 0) {
        panic();
    }
    else if (strcmp(user_cmd, "ISHII") == 0) {
        char *argument = space + 1;
        int a = atoi(argument);

        char *space2 = strchr(argument, ' ');
        char op = '+';
        int b = 0;

        if (space2 != NULL) {
            op = *(space2 + 1);

            char *space3 = strchr(space2 + 1, ' ');
            if (space3 != NULL) {
                b = atoi(space3 + 1);
            }
        }

        int result;
        switch (op) {
            case '-': result = a - b; break;
            case '*': result = a * b; break;
            case '/': result = a / b; break;
            default: result = a + b; break;
        }

        printf("\n%d\n", result);
    }
    else if (strcmp(user_cmd, "ASCII") == 0) {
    	chito_ascii();
    }
    else if (strcmp(user_cmd, "RUN") == 0) {
        char *argument = space + 1;
        int64_t size = chifs_size(argument);

        if (size < 0) {
            printf("\nFile not found: %s\n", argument);
        } else {
            char *buf = malloc((size_t)size + 1);
            int64_t got = chifs_read_file(argument, buf, (size_t)size);
            buf[got] = '\0';

            char *line_start = buf;
            int prev_interp = is_interp;

            is_interp = 1;

            while (line_start != NULL) {
                char *newline = strchr(line_start, '\n');

                if (newline != NULL) {
                    *newline = '\0';
                }

                if (*line_start != '\0') {
                    char script_cmd[256];

                    strncpy(script_cmd, line_start, sizeof script_cmd - 1);
                    script_cmd[sizeof script_cmd - 1] = '\0';

                    execute_command(script_cmd);
                }

                line_start = (newline != NULL) ? newline + 1 : NULL;
            }
            is_interp = prev_interp;

            free(buf);
        }
    }
    else {
        printf("\nUnknown command: %s\n", user_cmd);
    }
}

void split_with_strchr(char *str, char delim) {
    char *current = str;
    char *next;

    while (current != NULL && *current != '\0') {
        next = strchr(current, delim);

        if (next != NULL) {
            *next = '\0';
            printf("%s\n", current);
            current = next + 1;
        } else {
            printf("%s\n", current);
            break;
        }
    }
}

void kmain(void) {
    ksetup();

    list_disks();
    mount_filesystem();

    console_set_color(CONSOLE_YELLOW, CONSOLE_BLACK);
    printf("\nChiOS			~ Version 0.0.1 ~\n");

    console_set_color(CONSOLE_GREEN, CONSOLE_BLACK);

	chito_ascii();

	char user_cmd[256];

	cmd_in:
        console_set_color(CONSOLE_MAGENTA, CONSOLE_BLACK);
        if (is_interp == 0) {
        	printf("\nChiOS> ");
    	}
    	else {
    		printf("\n");
		}

        if (scanf(" %255[^\n]", user_cmd) != 1) {
            goto cmd_in;
        }

        getchar();

        execute_command(user_cmd);

    goto cmd_in;

    hcf();
}
