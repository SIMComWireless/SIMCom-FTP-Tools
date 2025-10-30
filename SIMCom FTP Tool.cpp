#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RING_BUFFER_SIZE 8192
#define MAX_PACKET_SIZE 8192
#define MAX_RESPONSE_SIZE 8192
// Maximum number of retries for the same offset when +CFTPSGET: 14 is returned
#define MAX_OFFSET_RETRIES 5


typedef struct {
    char buffer[RING_BUFFER_SIZE];
    int head;
    int tail;
    int count;
    CRITICAL_SECTION lock;
} RingBuffer;

typedef struct {
    HANDLE hCom;
    RingBuffer* rxBuffer;
    volatile int running;
} SerialPort;

// Ring buffer functions
void ring_buffer_init(RingBuffer* rb) {
    memset(rb->buffer, 0, RING_BUFFER_SIZE);
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    InitializeCriticalSection(&rb->lock);
}

int ring_buffer_put(RingBuffer* rb, char data) {
    EnterCriticalSection(&rb->lock);

    if (rb->count >= RING_BUFFER_SIZE) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }

    rb->buffer[rb->head] = data;
    rb->head = (rb->head + 1) % RING_BUFFER_SIZE;
    rb->count++;

    LeaveCriticalSection(&rb->lock);
    return 1;
}

// Bulk write 'len' bytes from src into ring buffer (returns bytes written)
int ring_buffer_put_bulk(RingBuffer* rb, const char* src, int len) {
    EnterCriticalSection(&rb->lock);
    if (len <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int freeSpace = RING_BUFFER_SIZE - rb->count;
    int toWrite = len > freeSpace ? freeSpace : len;
    if (toWrite <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    if (rb->head + toWrite <= RING_BUFFER_SIZE) {
        memcpy(rb->buffer + rb->head, src, toWrite);
        rb->head = (rb->head + toWrite) % RING_BUFFER_SIZE;
        rb->count += toWrite;
        LeaveCriticalSection(&rb->lock);
        return toWrite;
    }
    int first = RING_BUFFER_SIZE - rb->head;
    memcpy(rb->buffer + rb->head, src, first);
    int second = toWrite - first;
    if (second > 0) memcpy(rb->buffer, src + first, second);
    rb->head = (rb->head + toWrite) % RING_BUFFER_SIZE;
    rb->count += toWrite;
    LeaveCriticalSection(&rb->lock);
    return toWrite;
}

int ring_buffer_get(RingBuffer* rb, char* data) {
    EnterCriticalSection(&rb->lock);

    if (rb->count <= 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }

    *data = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    rb->count--;

    LeaveCriticalSection(&rb->lock);
    return 1;
}

int ring_buffer_available(RingBuffer* rb) {
    return rb->count;
}

// Peek at a byte at 'index' (0..count-1) from tail without removing it.
// Returns 1 on success and sets *out, 0 if index out of range.
int ring_buffer_peek(RingBuffer* rb, int index, char* out) {
    EnterCriticalSection(&rb->lock);
    if (index < 0 || index >= rb->count) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int pos = (rb->tail + index) % RING_BUFFER_SIZE;
    *out = rb->buffer[pos];
    LeaveCriticalSection(&rb->lock);
    return 1;
}

// Find first occurrence of 'ch' in buffer; returns zero-based index from tail or -1 if not found.
int ring_buffer_find_char(RingBuffer* rb, char ch) {
    EnterCriticalSection(&rb->lock);
    int cnt = rb->count;
    if (cnt <= 0) {
        LeaveCriticalSection(&rb->lock);
        return -1;
    }

    // If data is contiguous from tail, search in one block
    if (rb->tail + cnt <= RING_BUFFER_SIZE) {
        void* p = memchr(rb->buffer + rb->tail, (int)ch, (size_t)cnt);
        if (p) {
            int idx = (int)((char*)p - (rb->buffer + rb->tail));
            LeaveCriticalSection(&rb->lock);
            return idx;
        }
        LeaveCriticalSection(&rb->lock);
        return -1;
    }

    // Wrapped case: search first segment then second
    int first = RING_BUFFER_SIZE - rb->tail;
    void* p1 = memchr(rb->buffer + rb->tail, (int)ch, (size_t)first);
    if (p1) {
        int idx = (int)((char*)p1 - (rb->buffer + rb->tail));
        LeaveCriticalSection(&rb->lock);
        return idx;
    }
    int second = cnt - first;
    if (second > 0) {
        void* p2 = memchr(rb->buffer, (int)ch, (size_t)second);
        if (p2) {
            int idx = first + (int)((char*)p2 - rb->buffer);
            LeaveCriticalSection(&rb->lock);
            return idx;
        }
    }

    LeaveCriticalSection(&rb->lock);
    return -1;
}

// Read up to 'length' bytes from buffer into dest, removing them. Returns bytes read.
int ring_buffer_read_bulk(RingBuffer* rb, char* dest, int length) {
    EnterCriticalSection(&rb->lock);
    if (length <= 0 || rb->count == 0) {
        LeaveCriticalSection(&rb->lock);
        return 0;
    }
    int toRead = length;
    if (toRead > rb->count) toRead = rb->count;

    if (rb->tail + toRead <= RING_BUFFER_SIZE) {
        memcpy(dest, rb->buffer + rb->tail, toRead);
        rb->tail = (rb->tail + toRead) % RING_BUFFER_SIZE;
        rb->count -= toRead;
        LeaveCriticalSection(&rb->lock);
        return toRead;
    }

    int first = RING_BUFFER_SIZE - rb->tail;
    memcpy(dest, rb->buffer + rb->tail, first);
    int second = toRead - first;
    if (second > 0) memcpy(dest + first, rb->buffer, second);
    rb->tail = (rb->tail + toRead) % RING_BUFFER_SIZE;
    rb->count -= toRead;
    LeaveCriticalSection(&rb->lock);
    return toRead;
}

// Serial receive thread (uses OVERLAPPED asynchronous reads to reduce blocking)
DWORD WINAPI serial_receive_thread(LPVOID param) {
    SerialPort* serial = (SerialPort*)param;
    DWORD bytesRead = 0;
    char readBuffer[256];
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    while (serial->running) {
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(serial->hCom, readBuffer, sizeof(readBuffer), &bytesRead, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD wait = WaitForSingleObject(ov.hEvent, 500);
                if (wait == WAIT_OBJECT_0) {
                    GetOverlappedResult(serial->hCom, &ov, &bytesRead, FALSE);
                }
                else {
                    // timeout or other
                    bytesRead = 0;
                }
            }
            else {
                // immediate error
                Sleep(1);
                bytesRead = 0;
            }
        }

        if (bytesRead > 0) {
            int remaining = (int)bytesRead;
            char* ptr = readBuffer;
            while (remaining > 0) {
                int w = ring_buffer_put_bulk(serial->rxBuffer, ptr, remaining);
                if (w <= 0) {
                    // buffer full, wait for consumer
                    Sleep(1);
                    continue;
                }
                ptr += w;
                remaining -= w;
            }
        }
    }

    CloseHandle(ov.hEvent);
    return 0;
}

// Serial port functions
HANDLE open_serial_port(const char* portName, int baudRate) {
    HANDLE hCom;
    char fullPortName[20];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    sprintf_s(fullPortName, sizeof(fullPortName), "\\\\.\\%s", portName);

    // Open overlapped so we can do async I/O
    hCom = CreateFileA(fullPortName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (hCom == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    // Configure serial port parameters
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hCom, &dcb)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(hCom, &dcb)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    // Configure timeouts
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 10;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    if (!SetCommTimeouts(hCom, &timeouts)) {
        CloseHandle(hCom);
        return INVALID_HANDLE_VALUE;
    }

    return hCom;
}

int send_at_command(HANDLE hCom, const char* command) {
    DWORD bytesWritten = 0;
    char fullCommand[256];
    sprintf_s(fullCommand, sizeof(fullCommand), "%s\r\n", command);

    // Use OVERLAPPED WriteFile to avoid blocking the caller. We open the port with
    // FILE_FLAG_OVERLAPPED, so this will be asynchronous when needed.
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    DWORD len = (DWORD)strlen(fullCommand);

    BOOL ok = WriteFile(hCom, fullCommand, len, &bytesWritten, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            // wait for completion with a modest timeout
            DWORD wait = WaitForSingleObject(ov.hEvent, 2000);
            if (wait == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(hCom, &ov, &bytesWritten, FALSE)) {
                    CloseHandle(ov.hEvent);
                    return 0;
                }
            }
            else {
                // timeout or error; cancel pending I/O
                CancelIo(hCom);
                CloseHandle(ov.hEvent);
                return 0;
            }
        }
        else {
            CloseHandle(ov.hEvent);
            return 0;
        }
    }

    CloseHandle(ov.hEvent);
    return (bytesWritten == len);
}

// Read a line from the ring buffer
int read_line_from_buffer(RingBuffer* rb, char* buffer, int bufferSize) {
    // Find newline without removing bytes first
    int idx = ring_buffer_find_char(rb, '\n');
    if (idx == -1) return 0; // no complete line yet

    int toCopy = idx + 1; // include the '\n'
    if (toCopy > bufferSize - 1) toCopy = bufferSize - 1; // avoid overflow

    int n = ring_buffer_read_bulk(rb, buffer, toCopy);
    if (n <= 0) return 0;
    buffer[n] = '\0';
    return 1;
}


// Wait for a specific response
int wait_for_response(RingBuffer* rb, const char* expected, int timeout_ms) {
    char line[256];
    DWORD startTime = GetTickCount();

    while ((GetTickCount() - startTime) < (DWORD)timeout_ms) {
        if (read_line_from_buffer(rb, line, sizeof(line))) {
            printf("Received: %s", line);

            if (strstr(line, expected) != NULL) {
                return 1;
            }
        }
        Sleep(1);
    }
    return 0;
}

// Parse numeric response
int parse_number_response(RingBuffer* rb, const char* prefix, int* value, int timeout_ms) {
    char line[256];
    DWORD startTime = GetTickCount();

    while ((GetTickCount() - startTime) < (DWORD)timeout_ms) {
        if (read_line_from_buffer(rb, line, sizeof(line))) {
            printf("Received: %s", line);

            const char* pos = strstr(line, prefix);
            if (pos != NULL) {
                pos += strlen(prefix);
                while (*pos && !isdigit(*pos)) pos++;
                if (*pos) {
                    *value = atoi(pos);
                    return 1;
                }
            }
        }
        Sleep(1);
    }
    return 0;
}

// Enumerate available serial ports
void enumerate_serial_ports() {
    printf("Available serial ports:\n");

    for (int i = 1; i <= 20; i++) {
        char portName[20];
        HANDLE hCom;

        sprintf_s(portName, sizeof(portName), "COM%d", i);
        hCom = CreateFileA(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);

        if (hCom != INVALID_HANDLE_VALUE) {
            printf("  %s\n", portName);
            CloseHandle(hCom);
        }
    }
}

// Download file data
int download_file_data(HANDLE hCom, RingBuffer* rb, const char* filename, int total_size) {
    FILE* file;
    int offset = 0;
    int packet_size = 4096;
    char command[256];
    char line[256];
    int bytes_received = 0;

    if (fopen_s(&file, filename, "wb") != 0) {
        printf("Unable to create file %s\n", filename);
        return 0;
    }

    while (offset < total_size) {
        int current_size = (total_size - offset) > packet_size ? packet_size : (total_size - offset);
        int retries = 0;

        // Send download command
        sprintf_s(command, sizeof(command), "AT+CFTPSGET=\"%s\",%d,%d", filename, offset, current_size);
        if (!send_at_command(hCom, command)) {
            printf("Failed to send command\n");
            fclose(file);
            return 0;
        }

        int data_received = 0;
        int expecting_data = 1;

        while (expecting_data) {
            if (!read_line_from_buffer(rb, line, sizeof(line))) {
                Sleep(1);
                continue;
            }

            printf("Received: %s", line);

            if (strstr(line, "+CFTPSGET: DATA,") != NULL) {
                // Parse data length
                const char* data_pos = strstr(line, "DATA,");
                if (data_pos) {
                    data_pos += 5;
                    int data_len = atoi(data_pos);

                    if (data_len > 0) {
                        // Read binary data
                        char* data = (char*)malloc(data_len);
                        int bytes_read = 0;

                        while (bytes_read < data_len) {
                            if (ring_buffer_get(rb, &data[bytes_read])) {
                                bytes_read++;
                            }
                            else {
                                Sleep(1);
                            }
                        }

                        // Print 16-byte-per-line hex view with offset relative to bytes already received
                        for (int i = 0; i < data_len; ++i) {
                            if ((i % 16) == 0) {
                                // display the starting offset for this line
                                printf("\n%08X: ", bytes_received + i);
                            }
                            printf("%02X ", (unsigned char)data[i]);
                        }
                        printf("\n");

                        // Write to file
                        fwrite(data, 1, data_len, file);
                        fflush(file);

                        data_received += data_len;
                        bytes_received += data_len;
                        free(data);

                        printf("Received %d bytes, total progress: %d/%d (%.1f%%)\n",
                            data_len, bytes_received, total_size,
                            (float)bytes_received / total_size * 100);
                    }
                }
            }
            else if (strstr(line, "+CFTPSGET: 14") != NULL) {
                // Server returned code 14 for this offset — retry this offset
                printf("Server returned +CFTPSGET: 14 for offset %d — will retry (attempt %d/%d)\n", offset, retries + 1, MAX_OFFSET_RETRIES);
                retries++;
                if (retries >= MAX_OFFSET_RETRIES) {
                    printf("Exceeded max retries (%d) for offset %d, aborting.\n", MAX_OFFSET_RETRIES, offset);
                    fclose(file);
                    return 0;
                }
                // Stop waiting for DATA for this attempt; outer loop will re-send same offset
                expecting_data = 0;
                break;
            }
            else if (strstr(line, "+CFTPSGET: 0") != NULL) {
                expecting_data = 0;
                offset += data_received;
                break;
            }
            else if (strstr(line, "ERROR") != NULL) {
                printf("Download error\n");
                fclose(file);
                return 0;
            }
        }
    }

    fclose(file);
    printf("File download complete, total size: %d bytes\n", bytes_received);
    return 1;
}

int main(int argc, char** argv) {
    SerialPort serial;
    RingBuffer rxBuffer;
    HANDLE hThread;
    char portName[64];
    char input[100];
    int file_size = 0;

    // Command-line parameters (positional): <COM> <FTP_SERVER> <FTP_PORT> <USER> <PASS> <FILENAME>
    char ftp_server[128] = { 0 };
    int ftp_port = 0;
    char ftp_user[128] = { 0 };
    char ftp_pass[128] = { 0 };
    char ftp_filename[260] = { 0 };

    if (argc >= 7) {
        // argv[1] = COM (e.g., COM3)
        // Use snprintf to safely copy and truncate inputs while ensuring NUL-termination.
        snprintf(portName, sizeof(portName), "%s", argv[1]);
        snprintf(ftp_server, sizeof(ftp_server), "%s", argv[2]);
        ftp_port = atoi(argv[3]);
        snprintf(ftp_user, sizeof(ftp_user), "%s", argv[4]);
        snprintf(ftp_pass, sizeof(ftp_pass), "%s", argv[5]);
        snprintf(ftp_filename, sizeof(ftp_filename), "%s", argv[6]);
    }
    else {
        // interactive input (existing behavior)
        printf("=== SIMCOM FTP File Download Tool ===\n\n");
        // enumerate serial ports and prompt as before
        enumerate_serial_ports();
        printf("\nEnter COM port to use (e.g., COM3): ");
        fgets(portName, sizeof(portName), stdin);
        portName[strcspn(portName, "\r\n")] = 0;

        printf("Enter FTP server address (e.g., 117.131.85.140): ");
        fgets(ftp_server, sizeof(ftp_server), stdin);
        ftp_server[strcspn(ftp_server, "\r\n")] = 0;
        char portbuf[16];
        printf("Enter FTP server port (e.g., 60059): ");
        fgets(portbuf, sizeof(portbuf), stdin);
        ftp_port = atoi(portbuf);
        printf("Enter FTP username: ");
        fgets(ftp_user, sizeof(ftp_user), stdin);
        ftp_user[strcspn(ftp_user, "\r\n")] = 0;
        printf("Enter FTP password: ");
        fgets(ftp_pass, sizeof(ftp_pass), stdin);
        ftp_pass[strcspn(ftp_pass, "\r\n")] = 0;
        printf("Enter filename to download (e.g., starline_gen7v2_900-00624.bin): ");
        fgets(ftp_filename, sizeof(ftp_filename), stdin);
        ftp_filename[strcspn(ftp_filename, "\r\n")] = 0;
    }

    printf("=== SIMCOM FTP File Download Tool ===\n\n");

    // Enumerate serial ports
    enumerate_serial_ports();

    // Select COM port
    printf("\nEnter COM port to use (e.g., COM3): ");
    // If a COM port was provided on the command line, portName is already set; otherwise interactive input is available (the commented fgets can be re-enabled)
    // fgets(portName, sizeof(portName), stdin);
    // portName[strcspn(portName, "\r\n")] = 0;

    // Initialize ring buffer
    ring_buffer_init(&rxBuffer);

    // Open serial port
    printf("Opening serial port %s...\n", portName);
    serial.hCom = open_serial_port(portName, 115200);
    serial.rxBuffer = &rxBuffer;

    if (serial.hCom == INVALID_HANDLE_VALUE) {
        printf("Unable to open serial port %s\n", portName);
        return 1;
    }

    printf("Serial port opened successfully\n");

    // Start receiver thread
    serial.running = 1;
    hThread = CreateThread(NULL, 0, serial_receive_thread, &serial, 0, NULL);
    if (hThread == NULL) {
        printf("Unable to create receiver thread\n");
        CloseHandle(serial.hCom);
        return 1;
    }

    // Execute AT command sequence
    printf("\nStarting AT command sequence...\n");

    // 1. Send AT
    printf("\n1. Sending AT command...\n");
    if (!send_at_command(serial.hCom, "AT") || !wait_for_response(&rxBuffer, "OK", 1000)) {
        printf("AT command failed\n");
        goto cleanup;
    }

    // 2. Send AT+CFTPSSTART
    printf("\n2. Starting FTP service...\n");
    if (!send_at_command(serial.hCom, "AT+CFTPSSTART") || !wait_for_response(&rxBuffer, "+CFTPSSTART: 0", 5000)) {
        printf("Failed to start FTP service\n");
        goto cleanup;
    }

    // 3. Send AT+CFTPSSINGLEIP=1
    printf("\n3. Set single-IP mode...\n");
    if (!send_at_command(serial.hCom, "AT+CFTPSSINGLEIP=1") || !wait_for_response(&rxBuffer, "OK", 5000)) {
        printf("Failed to set single-IP mode\n");
        goto cleanup;
    }

    // 4. Send login command
    printf("\n4. Logging into FTP server...\n");
    {
        char loginCmd[512];
        // Construct login command using FTP parameters from CLI or interactive input
        sprintf_s(loginCmd, sizeof(loginCmd), "AT+CFTPSLOGIN=\"%s\",%d,\"%s\",\"%s\",0", ftp_server, ftp_port, ftp_user, ftp_pass);
        if (!send_at_command(serial.hCom, loginCmd) || !wait_for_response(&rxBuffer, "+CFTPSLOGIN: 0", 30000)) {
            printf("FTP login failed\n");
            goto cleanup;
        }
    }

    // 5. Set transfer type
    printf("\n5. Set transfer type...\n");
    if (!send_at_command(serial.hCom, "AT+CFTPSTYPE=I") || !wait_for_response(&rxBuffer, "+CFTPSTYPE: 0", 10000)) {
        printf("Failed to set transfer type\n");
        goto cleanup;
    }

    // 6. Get file size
    printf("\n6. Get file size...\n");
    char filename_command[256];
    // Use filename from CLI or interactive input
    sprintf_s(filename_command, sizeof(filename_command), "AT+CFTPSSIZE=\"%s\"", ftp_filename);
    if (!send_at_command(serial.hCom, filename_command) ||
        !parse_number_response(&rxBuffer, "+CFTPSSIZE: ", &file_size, 10000)) {
        printf("Failed to get file size\n");
        goto cleanup;
    }
    printf("Total file size: %d bytes\n", file_size);

    // 7. Download file
    printf("\n7. Start downloading file...\n");
    if (!download_file_data(serial.hCom, &rxBuffer, ftp_filename, file_size)) {
        printf("File download failed\n");
        goto cleanup;
    }

    printf("\n=== All operations completed ===\n");

cleanup:
    // Cleanup resources
    serial.running = 0;
    WaitForSingleObject(hThread, 1000);
    CloseHandle(hThread);
    CloseHandle(serial.hCom);
    DeleteCriticalSection(&rxBuffer.lock);

    printf("Press any key to exit...");
    getchar();

    return 0;
}