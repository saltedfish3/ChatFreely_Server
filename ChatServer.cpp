#include "curl/curl.h"
#include "network/ChatServerListener.h"
#include "global/GlobalVar.h"

int main()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    ChatServerListener csl = ChatServerListener(SPort,5,1);
    csl.start();

    return 0;
}
