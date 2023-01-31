#include <nfd.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* this test should compile on all supported platforms */

int main(void) {
    // initialize NFD
    // either call NFD_Init at the start of your program and NFD_Quit at the end of your program,
    // or before/after every time you want to show a file dialog.
    NFD_Init();

    NfdDialogParams params = {0};

    params.title = "this is a custom pick folder title";

    void* asyncOpHandle;
    params.outAsyncOpHandle = &asyncOpHandle;

    // show the dialog
    nfdresult_t result = NFD_PickFolderWin(&params);
    if (result == NFD_OKAY) {
        puts("Success!");
        while (!NFD_HasAsyncOpCompleted(asyncOpHandle)) {
            usleep(1000);
        }
        char* outPath;
        NfdDialogResponse response = {.outPath = &outPath};
        if ((result = NFD_GetAsyncOpResult(asyncOpHandle, &response)) == NFD_OKAY) {
            printf("path size = %zu\n", response.outPathSize);
            puts(outPath);
            // remember to free the memory (since NFD_OKAY is returned)
            NFD_FreePath(outPath);
        }
        else if (result == NFD_CANCEL)
            puts("User pressed cancel.");
        else
            printf("Error: %s\n", NFD_GetError());
        NFD_FreeHandle(asyncOpHandle);
    } else if (result == NFD_CANCEL) {
        puts("User pressed cancel.");
    } else {
        printf("Error: %s\n", NFD_GetError());
    }

    // Quit NFD
    NFD_Quit();

    return 0;
}
