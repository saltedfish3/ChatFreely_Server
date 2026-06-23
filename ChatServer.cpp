#include "network/ChatServerListener.h"

int main()
{
    ChatServerListener csl = ChatServerListener(9000,5,1);
    csl.start();
    return 0;
}
