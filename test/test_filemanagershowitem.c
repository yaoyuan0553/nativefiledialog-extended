//
// Created by yuan on 11/27/22.
//
#include <nfd.h>

#include <stdio.h>
#include <stdlib.h>

/* this test should compile on all supported platforms */

int main(void) {
    // initialize NFD
    // either call NFD_Init at the start of your program and NFD_Quit at the end of your program,
    // or before/after every time you want to show a file dialog.
    NFD_Init();

    NfdFileManagerParams params = {
//        "file:///home/yuan/Downloads/WeChatSetup.exe",
        "/home/yuan/Downloads",
//        "/home/yuan/.wine/dosdevices/c:/users/yuan/Documents/WeChat Files/wxid_vonu0ww7zfoh11/FileStorage/File/2022-11/backtrace2.txt",
//        "/home/yuan/Blah",
        NFD_FM_OPEN_FOLDER,
        1
    };

    // show the dialog
    nfdresult_t result = NFD_OpenFileManager(&params);
    if (result == NFD_OKAY) {
        puts("Success!");
        // remember to free the memory (since NFD_OKAY is returned)
    }
    else {
        printf("Error: %s\n", NFD_GetError());
    }

    // Quit NFD
    NFD_Quit();

    return 0;
}
