#include <iostream>
#include <vector>
#include <string>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <getopt.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdlib>

extern "C" {
#include <spandsp.h>
}

bool MODEM_BIT_REVERSED = false;
bool fax_finished = false;

static const int BLOCK = 160;
static const int RING_MIN = BLOCK * 2;
static const int RING_MAX = BLOCK * 10;

uint8_t reverse_bits(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

int16_t convert_sample_in(uint8_t b) {
    if (MODEM_BIT_REVERSED) b = reverse_bits(b);
    return ulaw_to_linear(b);
}

uint8_t convert_sample_out(int16_t s) {
    uint8_t b = linear_to_ulaw(s);
    if (MODEM_BIT_REVERSED) b = reverse_bits(b);
    return b;
}

void span_log_msg(void *user_data, int level, const char *text) {
    std::cout << "[spandsp] " << text << std::flush;
}

void phase_e_handler(void *user_data, int result) {
    std::cout << "\n[T.30] Final Status: " << t30_completion_code_to_str(result) << std::endl;
    fax_finished = true;
}

void handle_modem_event(uint8_t c) {
    switch (c) {
        case 'b':
            std::cout << "[Modem] BUSY.\n";
            fax_finished = true;
            break;
        case 0x03:
            std::cout << "[Modem] ETX.\n";
            fax_finished = true;
            break;
        default:
            std::cout << "[Modem] Event: 0x" << std::hex << (int)c << std::dec << "\n";
            break;
    }
}

bool is_pdf(const std::string &path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos + 1);
    for (auto &c : ext) c = (char)tolower((unsigned char)c);
    return ext == "pdf";
}

bool is_tiff(const std::string &path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos + 1);
    for (auto &c : ext) c = (char)tolower((unsigned char)c);
    return (ext == "tif" || ext == "tiff");
}

std::string page_size_to_gs(const std::string &size) {
    std::string s;
    for (auto c : size) s.push_back((char)tolower((unsigned char)c));
    if (s == "a4")    return "a4";
    if (s == "letter")return "letter";
    if (s == "legal") return "legal";
    if (s == "a3")    return "a3";
    return "a4";
}

std::string prepare_fax_file(const std::string &input_file,
                             bool low_res,
                             bool medium_res,
                             const std::string &page_size,
                             bool allow_oversize)
{
    if (!is_pdf(input_file))
        return input_file;

    std::string tmp_file = "/tmp/fax_convert_out.tif";
    std::cout << "[Converter] PDF detected. Converting to G4 TIFF via Ghostscript...\n";

    std::string gs_page = page_size_to_gs(page_size.empty() ? "a4" : page_size);
    std::string res = "204x196";
    if (low_res) res = "204x98";

    std::string cmd = "gs -q -dNOPAUSE -dBATCH -sDEVICE=tiffg4 ";
    cmd += "-sPAPERSIZE=" + gs_page + " ";
    cmd += "-r" + res + " ";
    if (allow_oversize) {
        cmd += "-dFIXEDMEDIA -dPDFFitPage ";
    }
    cmd += "-sOutputFile=" + tmp_file + " " + input_file + " 2>/dev/null";

    if (system(cmd.c_str()) == 0) {
        std::cout << "[Converter] Success: " << tmp_file << " created.\n";
        return tmp_file;
    } else {
        std::cerr << "[Converter] Error: Ghostscript failed.\n";
        return "";
    }
}

bool send_cmd(int fd, const char* cmd, const char* expect="OK", int timeout_ms=3000) {
    tcdrain(fd);
    usleep(50000);
    tcflush(fd, TCIOFLUSH);

    write(fd, cmd, strlen(cmd));

    std::string resp;
    char buf[256];
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        ssize_t n = read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = 0;
            resp += buf;

            if (resp.find(expect) != std::string::npos)
                return true;
        }
        usleep(50000);
        elapsed += 50;
    }

    return false;   // (Removed the "got: goo" debug line)
}

void print_help(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options] <file.pdf|file.tif>\n\n"
        << "Options:\n"
        << "  -d <num>        Dial number\n"
        << "  -u <id>         Set fax TSID / user identifier\n"
        << "  -P              Poll remote fax (receive)\n"
        << "  -E              Disable ECM\n"
        << "  -B <speed>      Max fax speed\n"
        << "  -s <size>       Page size: a4, letter, legal, a3\n"
        << "  -l              Low resolution (204x98)\n"
        << "  -m              Medium/Fine resolution (204x196)\n"
        << "  -G              Allow oversized pages\n"
        << "  -h, --help      Show help\n";
}

int main(int argc, char *argv[]) {
    std::string dial_num;
    std::string user_ident = "FAX";
    bool use_ecm = true;
    bool polling = false;
    int max_speed = 14400;
    std::string page_size = "a4";
    bool low_res = false;
    bool medium_res = false;
    bool allow_oversize = false;
    std::string temp_tif_path;

    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--help") == 0) { print_help(argv[0]); return 0; }

    int opt;
    while ((opt = getopt(argc, argv, "d:u:EB:Ps:lmGh")) != -1) {
        switch (opt) {
            case 'd': dial_num = optarg; break;
            case 'u': user_ident = optarg; break;
            case 'E': use_ecm = false; break;
            case 'B': max_speed = std::stoi(optarg); break;
            case 'P': polling = true; break;
            case 's': page_size = optarg; break;
            case 'l': low_res = true; medium_res = false; break;
            case 'm': medium_res = true; low_res = false; break;
            case 'G': allow_oversize = true; break;
            case 'h': print_help(argv[0]); return 0;
            default: print_help(argv[0]); return 1;
        }
    }

    if (dial_num.empty()) {
        std::cerr << "Error: -d <num> is required.\n";
        return 1;
    }

    if (!low_res && !medium_res)
        medium_res = true;

    if (!polling && optind >= argc) {
        std::cerr << "Error: No input file.\n";
        return 1;
    }

    std::string input_path;
    if (!polling) {
        input_path = argv[optind];
        if (!is_pdf(input_path) && !is_tiff(input_path)) {
            std::cerr << "Error: Input must be PDF or TIFF.\n";
            return 1;
        }
    }

    fax_state_t *fax_obj = fax_init(NULL, true);
    t30_state_t *t30_obj = fax_get_t30_state(fax_obj);

    logging_state_t *logging = t30_get_logging_state(t30_obj);
    span_log_set_message_handler(logging, span_log_msg, NULL);
    span_log_set_level(logging, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);

    t30_set_supported_modems(t30_obj, T30_SUPPORT_V27TER | T30_SUPPORT_V29 | T30_SUPPORT_V17);
    t30_set_supported_compressions(t30_obj, 0x01 | 0x02 | 0x04);

    int res_mask = low_res ? T4_RESOLUTION_R8_STANDARD : T4_RESOLUTION_R8_FINE;
    int width_mask = T4_SUPPORT_WIDTH_215MM;
    int image_caps = width_mask | res_mask | T4_SUPPORT_LENGTH_UNLIMITED;
    t30_set_supported_image_sizes(t30_obj, image_caps);

    t30_set_ecm_capability(t30_obj, use_ecm);
    t30_set_phase_e_handler(t30_obj, phase_e_handler, NULL);

    // Apply -u TSID
    t30_set_tx_ident(t30_obj, user_ident.c_str());
    t30_set_tx_sub_address(t30_obj, user_ident.c_str());

    if (polling) {
        t30_set_rx_file(t30_obj, "polled_fax.tif", -1);
    } else {
        std::string final_path = prepare_fax_file(input_path, low_res, medium_res, page_size, allow_oversize);
        if (final_path.empty()) return 1;
        if (final_path != input_path) temp_tif_path = final_path;
        t30_set_tx_file(t30_obj, final_path.c_str(), -1, -1);
    }

    int fd = open("/dev/ttyACM0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("Open /dev/ttyACM0"); return 1; }

    struct termios tty{};
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    tty.c_cflag |= CRTSCTS;
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tcsetattr(fd, TCSANOW, &tty);

    send_cmd(fd, "ATZ\r");
    send_cmd(fd, "AT+FCLASS=8\r");
    send_cmd(fd, "AT+VSM=131,8000\r");
    send_cmd(fd, "AT+VLS=1\r");

    std::cout << "Dialing " << dial_num << "...\n";
    send_cmd(fd, ("ATD" + dial_num + "\r").c_str(), "OK", 15000);

    if (!send_cmd(fd, "AT+VTR\r", "CONNECT", 5000)) {
        std::cerr << "Voice mode failed.\n";
        return 1;
    }

    std::vector<int16_t> ring;
    ring.reserve(RING_MAX);
    uint8_t serial_buf[1024];
    bool escaped = false;

    struct pollfd fds[1];
    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLOUT;

    while (!fax_finished) {
        int poll_ret = poll(fds, 1, 20);
        if (poll_ret < 0) break;

        if (fds[0].revents & POLLIN) {
            ssize_t n = read(fd, serial_buf, sizeof(serial_buf));
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    uint8_t c = serial_buf[i];
                    if (escaped) {
                        if (c == 0x10)
                            ring.push_back(convert_sample_in(0x10));
                        else
                            handle_modem_event(c);
                        escaped = false;
                    } else if (c == 0x10) {
                        escaped = true;
                    } else {
                        ring.push_back(convert_sample_in(c));
                    }
                }
            }
        }

        if (ring.size() > RING_MAX) {
            ring.erase(ring.begin(), ring.begin() + (ring.size() - RING_MAX));
        }

        while (ring.size() >= BLOCK) {
            int16_t rx_block[BLOCK], tx_block[BLOCK];
            memcpy(rx_block, ring.data(), BLOCK * sizeof(int16_t));
            ring.erase(ring.begin(), ring.begin() + BLOCK);

            fax_rx(fax_obj, rx_block, BLOCK);
            int tx_len = fax_tx(fax_obj, tx_block, BLOCK);

            if (tx_len > 0 && (fds[0].revents & POLLOUT)) {
                std::vector<uint8_t> out;
                out.reserve(tx_len * 2);
                for (int j = 0; j < tx_len; j++) {
                    uint8_t b = convert_sample_out(tx_block[j]);
                    out.push_back(b);
                    if (b == 0x10) out.push_back(0x10);
                }
                write(fd, out.data(), out.size());
            }
        }
    }

    write(fd, "\x10\x03", 2);
    usleep(500000);
    send_cmd(fd, "ATH\r");

    fax_release(fax_obj);
    close(fd);

    if (!temp_tif_path.empty()) {
        unlink(temp_tif_path.c_str());
        std::cout << "[Converter] Temporary file removed.\n";
    }

    std::cout << "Done.\n";
    return 0;
}
