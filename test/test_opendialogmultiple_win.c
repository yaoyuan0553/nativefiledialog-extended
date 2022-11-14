//
// Created by yuan on 22-11-8.
//
#include <nfd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* this test should compile on all supported platforms */

int main(void) {
    // initialize NFD
    // either call NFD_Init at the start of your program and NFD_Quit at the end of your program,
    // or before/after every time you want to show a file dialog.
    NFD_Init();

    char* outPath;

    NfdDialogParams params = {0};
//    params.winFilter = "All\0*.*\0Text\0*.TXT\0Text no asterisk\0*.txt\0\0";
    params.outPath = &outPath;
    params.winFilter = "All\0*.*\0Text\0*.TXT\0C/C++ files\0*.c;*.cpp;*.cc\0Image Files\0*.jpg;*.png;*.jpeg\0\0";
    params.filterIndex = 1;
    params.title = "this is a custom title";

    // show the dialog
    nfdresult_t result = NFD_OpenDialogMultipleWin(&params);

    if (result == NFD_OKAY) {
        puts("Success!\n");
        printf("path size = %zu\n", params.outPathSize);
        const char* curPath = outPath;
        int i = 0;
        while (*curPath)
        {
            printf("path %d: %s\n", ++i, curPath);
            curPath += strlen(curPath) + 1;
        }
        NFD_FreePathN(outPath);
    } else if (result == NFD_CANCEL) {
        puts("User pressed cancel.");
    } else {
        printf("Error: %s\n", NFD_GetError());
    }

    // Quit NFD
    NFD_Quit();

    return 0;
}
