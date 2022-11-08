//
// Created by yuan on 22-11-7.
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

    char* outPath;

    NfdOpenDialogParams params = {0};
//    params.winFilter = "All\0*.*\0Text\0*.TXT\0Text no asterisk\0*.txt\0\0";
    params.outPath = &outPath;
    params.winFilter = "All\0*.*\0Text\0*.TXT\0C/C++ files\0*.c;*.cpp;*.cc\0Image Files\0*.jpg;*.png;*.jpeg\0\0";
    params.filterIndex = 1;
    params.title = "this is a custom title";

    // show the dialog
    nfdresult_t result = NFD_OpenDialogWin(&params);
    if (result == NFD_OKAY) {
        puts("Success!");
        printf("path size = %zu\n", params.outPathSize);
        puts(*params.outPath);
        // remember to free the memory (since NFD_OKAY is returned)
        NFD_FreePath(*params.outPath);
    } else if (result == NFD_CANCEL) {
        puts("User pressed cancel.");
    } else {
        printf("Error: %s\n", NFD_GetError());
    }

    // Quit NFD
    NFD_Quit();

    return 0;
}
