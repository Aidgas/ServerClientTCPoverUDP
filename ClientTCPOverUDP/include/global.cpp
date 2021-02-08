#include "global.h"

string genUuid()
{
    srand (clock());
    string GUID;
    int t = 0;
    char *szTemp = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    char *szHex  = "0123456789ABCDEF";
    char *szHex2 = "FEDCBA9876543210";
    int nLen = strlen(szTemp);
    
    for (t=0; t < nLen; t++)
    {
        int r = rand() % 15;
        char c = szTemp[t];

        switch(c)
        {
            case 'x' : { c = szHex[r]; } break;
            case 'y' : { c = szHex2[r]; } break;
            default: break;
        }

        GUID += c;
    }
    
    return GUID;
}