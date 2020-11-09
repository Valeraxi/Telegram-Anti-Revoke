﻿#include "Header.h"
#include "Telegram.h"
#include "Global.h"
#include "AntiRevoke.h"
#include "Updater.h"
#include "ILogger.h"


namespace g
{
    ULONG_PTR MainModule = NULL;
    ULONG CurrentVersion = 0;

    fntMalloc fnMalloc = NULL;
    fntFree fnFree = NULL;
    fntFree fnOriginalFree = NULL;
    fntIndex fnEditedIndex = NULL;
    fntIndex fnSignedIndex = NULL;
    fntIndex fnReplyIndex = NULL;
    // fntGetCurrentInstance fnGetCurrentInstance = NULL;
    LanguageInstance **ppLangInstance = NULL;
    PVOID RevokeByServer = NULL;
    PVOID OriginalRevoke = NULL;

    mutex Mutex;
    set<HistoryMessage*> RevokedMessages;

    namespace Offsets
    {
        ULONG TimeText;
        ULONG TimeWidth;
        ULONG MainView;
        ULONG Media;
        ULONG SignedTimeText;
        ULONG HistoryPeer;

        ULONG Index_toHistoryMessage;
    }

    /*
        English
            GetId         : en
            GetPluralId   : en
            GetName       : English
            GetNativeName : English

        Simplified Chinese
            GetId         : classic-zh-cn
            GetPluralId   : zh
            GetName       : Chinese (Simplified, @zh_CN)
            GetNativeName : [bad string]

        Traditional Chinese
            GetId         : zhhant-hk
            GetPluralId   : zh
            GetName       : Chinese (Traditional, Hong Kong)
            GetNativeName : [bad string]

        Japanese
            GetId         : ja-raw
            GetPluralId   : ja
            GetName       : Japanese
            GetNativeName : [bad string]

        Korean
            GetId         : ko
            GetPluralId   : ko
            GetName       : Korean
            GetNativeName : [bad string]

        So we use PluralId and Name.
    */
    map<wstring, vector<MARK_INFO>> MultiLanguageMarks =
    {
        {
            L"en",
            {
                { L"English", L"deleted ", 8 * 6 }
            }
        },

        {
            L"zh",
            {
                { L"Simplified", L"已删除 ", 7 * 6 },
                { L"Traditional", L"已刪除 ", 7 * 6 },
                { L"Cantonese", L"刪咗 ", 5 * 6 }			// Thanks @Rongronggg9, #29
            }
        },

        {
            L"ja",
            {
                { L"Japanese", L"削除された ", 11 * 6 }
            }
        },

        {
            L"ko",
            {
                { L"Korean", L"삭제 ", 5 * 6 }
            }
        }

        // For more languages or corrections, please go to the GitHub issue submission.
    };

    MARK_INFO CurrentMark = MultiLanguageMarks[L"en"][0];
};


BOOLEAN HookRevoke(BOOLEAN Status)
{
    PVOID HookAddress = g::RevokeByServer;
    PVOID TargetAddress = NULL;
    vector<BYTE> Shellcode;

    if (Status)
    {
        // Enable Hook
        // Save the original revoke function.
        g::OriginalRevoke = (PVOID)((ULONG_PTR)HookAddress + 5 + *(INT*)((ULONG_PTR)HookAddress + 1));

        TargetAddress = Utils::GetFunctionAddress(&Session::ProcessRevoke);
    }
    else
    {
        // Restore hook
        TargetAddress = g::OriginalRevoke;
    }

    Shellcode = Memory::MakeCall(HookAddress, TargetAddress);

    return Memory::ForceOperate(HookAddress, Shellcode.size(), [&]()
    {
        RtlCopyMemory(HookAddress, Shellcode.data(), Shellcode.size());
    });
}

BOOLEAN HookMemoryFree(BOOLEAN Status)
{
    if (Status)
    {
        // Enable Hook
        if (MH_CreateHook(g::fnFree, DetourFree, (PVOID*)&g::fnOriginalFree) != MH_OK) {
            return FALSE;
        }

        return MH_EnableHook(g::fnFree) == MH_OK;
    }
    else
    {
        // Restore hook
        return MH_DisableHook(g::fnFree) == MH_OK;
    }
}

void InitMarkLanguage()
{
    auto &Logger = ILogger::GetInstance();

    Safe::TryExcept([&]()
    {
        // LanguageInstance *Instance = g::fnGetCurrentInstance();
        LanguageInstance *pLangInstance = *g::ppLangInstance;
        if (pLangInstance == NULL) {
            Logger.TraceWarn("Get language instance failed.");
            return;
        }

        //printf("pLangInstance : %p\n", pLangInstance);
        //printf("GetId         : %ws\n", Instance->GetId()->GetText());
        //printf("GetPluralId   : %ws\n", Instance->GetPluralId()->GetText());
        //printf("GetName       : %ws\n", Instance->GetName()->GetText());
        //printf("GetNativeName : %ws\n", Instance->GetNativeName()->GetText());

        wstring CurrentPluralId = pLangInstance->GetPluralId()->GetText();
        wstring CurrentName = pLangInstance->GetName()->GetText();

        // Fix for irregularly named language packages
        if (CurrentPluralId == L"yue" && CurrentName == L"Cantonese") {
            CurrentPluralId = L"zh";
        }

        // find language
        auto Iterator = g::MultiLanguageMarks.find(CurrentPluralId);
        if (Iterator == g::MultiLanguageMarks.end()) {
            Logger.TraceWarn(string("An unadded language. PluralId: [") + Convert::UnicodeToAnsi(CurrentPluralId + wstring(L"] Name: [") + CurrentName) + string("]"));
            return;
        }

        const vector<MARK_INFO> &Sublanguages = Iterator->second;

        // default sublanguage
        g::CurrentMark = Sublanguages[0];

        // multiple sublanguages
        if (Sublanguages.size() > 1)
        {
            for (const MARK_INFO &Language : Sublanguages)
            {
                if (CurrentName.find(Language.LangName) != wstring::npos) {
                    // found sub language
                    g::CurrentMark = Language;
                    break;
                }
            }
        }

    }, [&](ULONG ExceptionCode)
    {
        Logger.TraceWarn("Function: [" __FUNCTION__ "] An exception was caught. Code: [" + Text::Format("0x%x", ExceptionCode) + "]");
    });
}

BOOLEAN SearchSigns()
{
    auto &Logger = ILogger::GetInstance();

    // Some of the following instructions are taken from version 1.8.8
    // Thanks to [采蘑菇的小蘑菇] for providing help with compiling Telegram.

    MODULEINFO MainModuleInfo = { 0 };
    if (!GetModuleInformation(GetCurrentProcess(), (HMODULE)g::MainModule, &MainModuleInfo, sizeof(MainModuleInfo))) {
        return FALSE;
    }

    {
        /*
            void __cdecl __std_exception_copy(__std_exception_data *from, __std_exception_data *to)

            .text:01B7CAAE 8A 01                                   mov     al, [ecx]
            .text:01B7CAB0 41                                      inc     ecx
            .text:01B7CAB1 84 C0                                   test    al, al
            .text:01B7CAB3 75 F9                                   jnz     short loc_1B7CAAE
            .text:01B7CAB5 2B CA                                   sub     ecx, edx
            .text:01B7CAB7 53                                      push    ebx
            .text:01B7CAB8 56                                      push    esi
            .text:01B7CAB9 8D 59 01                                lea     ebx, [ecx+1]
            .text:01B7CABC 53                                      push    ebx             ; size

            // find this (internal malloc)
            .text:01B7CABD E8 87 98 00 00                          call    _malloc

            .text:01B7CAC2 8B F0                                   mov     esi, eax
            .text:01B7CAC4 59                                      pop     ecx
            .text:01B7CAC5 85 F6                                   test    esi, esi
            .text:01B7CAC7 74 19                                   jz      short loc_1B7CAE2
            .text:01B7CAC9 FF 37                                   push    dword ptr [edi] ; source
            .text:01B7CACB 53                                      push    ebx             ; size_in_elements
            .text:01B7CACC 56                                      push    esi             ; destination
            .text:01B7CACD E8 B2 9E 01 00                          call    _strcpy_s
            .text:01B7CAD2 8B 45 0C                                mov     eax, [ebp+to]
            .text:01B7CAD5 8B CE                                   mov     ecx, esi
            .text:01B7CAD7 83 C4 0C                                add     esp, 0Ch
            .text:01B7CADA 33 F6                                   xor     esi, esi
            .text:01B7CADC 89 08                                   mov     [eax], ecx
            .text:01B7CADE C6 40 04 01                             mov     byte ptr [eax+4], 1
            .text:01B7CAE2
            .text:01B7CAE2                         loc_1B7CAE2:                            ; CODE XREF: ___std_exception_copy+2F↑j
            .text:01B7CAE2 56                                      push    esi             ; block

            // and find this (internal free)
            .text:01B7CAE3 E8 7F 45 00 00                          call    _free

            .text:01B7CAE8 59                                      pop     ecx
            .text:01B7CAE9 5E                                      pop     esi
            .text:01B7CAEA 5B                                      pop     ebx
            .text:01B7CAEB EB 0B                                   jmp     short loc_1B7CAF8

            malloc		41 84 C0 75 F9 2B CA 53 56 8D 59 01 53 E8
            free		56 E8 ?? ?? ?? ?? 59 5E 5B EB
        */
        vector<PVOID> vCallMalloc = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x41\x84\xC0\x75\xF9\x2B\xCA\x53\x56\x8D\x59\x01\x53\xE8", "xxxxxxxxxxxxxx");
        if (vCallMalloc.size() != 1) {
            Logger.TraceWarn("Search malloc falied.");
            return FALSE;
        }

        vector<PVOID> vCallFree = Memory::FindPatternEx(GetCurrentProcess(), vCallMalloc[0], 0x50, "\x56\xE8\x00\x00\x00\x00\x59\x5E\x5B\xEB", "xx????xxxx");
        if (vCallFree.size() != 1) {
            Logger.TraceWarn("Search free falied.");
            return FALSE;
        }

        ULONG_PTR CallMalloc = (ULONG_PTR)vCallMalloc[0];
        ULONG_PTR CallFree = (ULONG_PTR)vCallFree[0];

        g::fnMalloc = (fntMalloc)(CallMalloc + 18 + *(INT*)(CallMalloc + 14));
        g::fnFree = (fntFree)(CallFree + 6 + *(INT*)(CallFree + 2));

    }

    {
        /*
            void __userpurge Data::Session::processMessagesDeleted(Data::Session *this@<ecx>, int a2@<ebp>, int a3@<edi>, int a4@<esi>, int channelId, QVector<MTPint> *data)

            .text:008CD8C1 8B 08                                   mov     this, [eax]
            .text:008CD8C3 8B 45 E8                                mov     eax, [ebp-18h]
            .text:008CD8C6 3B 48 04                                cmp     this, [eax+4]
            .text:008CD8C9 74 41                                   jz      short loc_8CD90C
            .text:008CD8CB 8B 49 0C                                mov     this, [this+0Ch]
            .text:008CD8CE 51                                      push    this            ; item
            .text:008CD8CF 8B C4                                   mov     eax, esp
            .text:008CD8D1 8B 71 10                                mov     esi, [this+10h]
            .text:008CD8D4 89 08                                   mov     [eax], this
            .text:008CD8D6 85 C9                                   test    this, this
            .text:008CD8D8 0F 84 A5 00 00 00                       jz      loc_8CD983
            .text:008CD8DE 8B 4D E0                                mov     this, [ebp-20h] ; this

            // find this
            .text:008CD8E1 E8 9A 02 00 00                          call    ?destroyMessage@Session@Data@@QAEXV?$not_null@PAVHistoryItem@@@gsl@@@Z ; Data::Session::destroyMessage(gsl::not_null<HistoryItem *>)

            .text:008CD8E6 85 F6                                   test    esi, esi
            .text:008CD8E8 0F 84 0F 01 00 00                       jz      loc_8CD9FD
            .text:008CD8EE 80 BE 60 01 00 00 00                    cmp     byte ptr [esi+160h], 0
            .text:008CD8F5 75 69                                   jnz     short loc_8CD960
            .text:008CD8F7 8D 45 E4                                lea     eax, [ebp-1Ch]
            .text:008CD8FA 89 75 E4                                mov     [ebp-1Ch], esi
            .text:008CD8FD 50                                      push    eax             ; value
            .text:008CD8FE 8D 45 C8                                lea     eax, [ebp-38h]
            .text:008CD901 50                                      push    eax             ; result
            .text:008CD902 8D 4D B4                                lea     this, [ebp-4Ch] ; this
            .text:008CD905 E8 B6 3B D5 FF                          call    ?insert@?$flat_set@V?$not_null@PAVHistory@@@gsl@@U?$less@X@std@@@base@@QAE?AU?$pair@V?$flat_multi_set_iterator_impl@V?$not_null@PAVHistory@@@gsl@@V?$_Deque_iterator@V?$_Deque_val@U?$_Deque_simple_types@V?$flat_multi_set_const_wrap@V?$not_null@PAVHistory@@@gsl@@@base@@@std@@@std@@@std@@@base@@_N@std@@$$QAV?$not_null@PAVHistory@@@gsl@@@Z ; base::flat_set<gsl::not_null<History *>,std::less<void>>::insert(gsl::not_null<History *> &&)
            .text:008CD90A EB 54                                   jmp     short loc_8CD960

            8B 71 ?? 89 08 85 C9 0F 84 ?? ?? ?? ?? ?? ?? ?? E8

            ==========

            1.9.15 new :

            Telegram.exe+54069F - 51                    - push ecx
            Telegram.exe+5406A0 - 8B C4                 - mov eax,esp
            Telegram.exe+5406A2 - 89 08                 - mov [eax],ecx
            Telegram.exe+5406A4 - 8B CE                 - mov ecx,esi
            Telegram.exe+5406A6 - E8 D5751B00           - call Telegram.exe+6F7C80
            Telegram.exe+5406AB - 80 BE 58010000 00     - cmp byte ptr [esi+00000158],00 { 0 }
            Telegram.exe+5406B2 - 75 13                 - jne Telegram.exe+5406C7

            51 8B C4 89 08 8B CE E8 ?? ?? ?? ?? 80 BE ?? ?? ?? ?? 00
        */

        // ver < 1.9.15
        if (g::CurrentVersion < 1009015)
        {
            vector<PVOID> vCallDestroyMessage = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x8B\x71\x00\x89\x08\x85\xC9\x0F\x84\x00\x00\x00\x00\x00\x00\x00\xE8", "xx?xxxxxx???????x");
            if (vCallDestroyMessage.size() != 1) {
                Logger.TraceWarn("Search DestroyMessage falied.");
                return FALSE;
            }

            ULONG_PTR CallDestroyMessage = (ULONG_PTR)vCallDestroyMessage[0];
            g::RevokeByServer = (PVOID)(CallDestroyMessage + 16);
        }
        // ver >= 1.9.15
        else if (g::CurrentVersion >= 1009015)
        {
            vector<PVOID> vCallDestroyMessage = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x51\x8B\xC4\x89\x08\x8B\xCE\xE8\x00\x00\x00\x00\x80\xBE\x00\x00\x00\x00\x00", "xxxxxxxx????xx????x");
            if (vCallDestroyMessage.size() != 1) {
                Logger.TraceWarn("Search new DestroyMessage falied.");
                return FALSE;
            }

            ULONG_PTR CallDestroyMessage = (ULONG_PTR)vCallDestroyMessage[0];
            g::RevokeByServer = (PVOID)(CallDestroyMessage + 7);
        }
    }

    {
        /*
            void __thiscall HistoryMessage::applyEdition(HistoryMessage *this, MTPDmessage *message)

            .text:00A4F320 55                                      push    ebp
            .text:00A4F321 8B EC                                   mov     ebp, esp
            .text:00A4F323 6A FF                                   push    0FFFFFFFFh
            .text:00A4F325 68 28 4F C8 01                          push    offset __ehhandler$?applyEdition@HistoryMessage@@UAEXABVMTPDmessage@@@Z
            .text:00A4F32A 64 A1 00 00 00 00                       mov     eax, large fs:0
            .text:00A4F330 50                                      push    eax
            .text:00A4F331 83 EC 0C                                sub     esp, 0Ch
            .text:00A4F334 53                                      push    ebx
            .text:00A4F335 56                                      push    esi
            .text:00A4F336 57                                      push    edi
            .text:00A4F337 A1 04 68 ED 02                          mov     eax, ___security_cookie
            .text:00A4F33C 33 C5                                   xor     eax, ebp
            .text:00A4F33E 50                                      push    eax
            .text:00A4F33F 8D 45 F4                                lea     eax, [ebp+var_C]
            .text:00A4F342 64 A3 00 00 00 00                       mov     large fs:0, eax
            .text:00A4F348 8B D9                                   mov     ebx, this
            .text:00A4F34A 8B 7D 08                                mov     edi, [ebp+message]
            .text:00A4F34D 8B 77 08                                mov     esi, [edi+8]
            .text:00A4F350 8D 47 48                                lea     eax, [edi+48h]

            .text:00A4F353 81 E6 00 80 00 00                       and     esi, 8000h
            .text:00A4F359 F7 DE                                   neg     esi
            .text:00A4F35B 1B F6                                   sbb     esi, esi
            .text:00A4F35D 23 F0                                   and     esi, eax
            .text:00A4F35F 74 65                                   jz      short loc_A4F3C6
            .text:00A4F361 81 4B 18 00 80 00 00                    or      dword ptr [ebx+18h], 8000h
            .text:00A4F368 8B 43 08                                mov     eax, [ebx+8]
            .text:00A4F36B 8B 38                                   mov     edi, [eax]

            // find this (RuntimeComponent<HistoryMessageEdited,HistoryItem>::Index()
            .text:00A4F36D E8 6E 3A EA FF                          call    ?Index@?$RuntimeComponent@UHistoryMessageEdited@@VHistoryItem@@@@SAHXZ ; RuntimeComponent<HistoryMessageEdited,HistoryItem>::Index(void)

            .text:00A4F372 83 7C 87 08 04                          cmp     dword ptr [edi+eax*4+8], 4
            .text:00A4F377 73 28                                   jnb     short loc_A4F3A1
            .text:00A4F379 E8 62 3A EA FF                          call    ?Index@?$RuntimeComponent@UHistoryMessageEdited@@VHistoryItem@@@@SAHXZ ; RuntimeComponent<HistoryMessageEdited,HistoryItem>::Index(void)
            .text:00A4F37E 33 D2                                   xor     edx, edx

            E8 ?? ?? ?? ?? 83 7C 87 ?? ?? 73 ?? E8
        */
        vector<PVOID> vCallApplyEdition = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\xE8\x00\x00\x00\x00\x83\x7C\x87\x00\x00\x73\x00\xE8", "x????xxx??x?x");
        if (vCallApplyEdition.size() != 1) {
            Logger.TraceWarn("Search ApplyEdition falied.");
            return FALSE;
        }

        ULONG_PTR CallApplyEdition = (ULONG_PTR)vCallApplyEdition[0];
        g::fnEditedIndex = (fntIndex)(CallApplyEdition + 5 + *(INT*)(CallApplyEdition + 1));
    }

    {
        /*
            HistoryView__Message__refreshEditedBadge

            .text:009F109D                         loc_9F109D:                             ; CODE XREF: HistoryView__Message__refreshEditedBadge+69↑j
            .text:009F109D 8D 47 28                                lea     eax, [edi+28h]
            .text:009F10A0 50                                      push    eax
            .text:009F10A1 8D 4D E8                                lea     ecx, [ebp-18h]
            .text:009F10A4 E8 07 2E FC 00                          call    QDateTime__QDateTime
            .text:009F10A9 68 8C 88 3D 03                          push    offset gTimeFormat
            .text:009F10AE 8D 45 F0                                lea     eax, [ebp-10h]
            .text:009F10B1 C7 45 FC 00 00 00 00                    mov     dword ptr [ebp-4], 0
            .text:009F10B8 50                                      push    eax
            .text:009F10B9 8D 4D E8                                lea     ecx, [ebp-18h]
            .text:009F10BC E8 DF 91 FC 00                          call    QDateTime__toString
            .text:009F10C1 8D 4D E8                                lea     ecx, [ebp-18h]
            .text:009F10C4 C6 45 FC 02                             mov     byte ptr [ebp-4], 2
            .text:009F10C8 E8 43 31 FC 00                          call    QDateTime___QDateTime
            .text:009F10CD 8B 4D EC                                mov     ecx, [ebp-14h]
            .text:009F10D0 85 C9                                   test    ecx, ecx
            .text:009F10D2 74 12                                   jz      short loc_9F10E6
            .text:009F10D4 85 DB                                   test    ebx, ebx
            .text:009F10D6 0F 95 C0                                setnz   al
            .text:009F10D9 0F B6 C0                                movzx   eax, al
            .text:009F10DC 50                                      push    eax
            .text:009F10DD 8D 45 F0                                lea     eax, [ebp-10h]
            .text:009F10E0 50                                      push    eax
            .text:009F10E1 E8 AA BA 03 00                          call    HistoryMessageEdited__refresh
            .text:009F10E6
            .text:009F10E6                         loc_9F10E6:                             ; CODE XREF: HistoryView__Message__refreshEditedBadge+A2↑j
            .text:009F10E6 8B 46 08                                mov     eax, [esi+8]
            .text:009F10E9 8B 38                                   mov     edi, [eax]

            // find this (RuntimeComponent<HistoryMessageSigned,HistoryItem>::Index()
            .text:009F10EB E8 30 62 FA FF                          call    RuntimeComponent_HistoryMessageSigned_HistoryItem___Index

            .text:009F10F0 8B 44 87 08                             mov     eax, [edi+eax*4+8]
            .text:009F10F4 83 CF FF                                or      edi, 0FFFFFFFFh
            .text:009F10F7 83 F8 04                                cmp     eax, 4
            .text:009F10FA 0F 82 F2 00 00 00                       jb      loc_9F11F2
            .text:009F1100 8B 76 08                                mov     esi, [esi+8]
            .text:009F1103 03 F0                                   add     esi, eax
            .text:009F1105 0F 84 E7 00 00 00                       jz      loc_9F11F2
            .text:009F110B 8B 45 EC                                mov     eax, [ebp-14h]
            .text:009F110E 85 C0                                   test    eax, eax
            .text:009F1110 74 1D                                   jz      short loc_9F112F
            .text:009F1112 85 DB                                   test    ebx, ebx
            .text:009F1114 74 19                                   jz      short loc_9F112F
            .text:009F1116 FF 35 74 3C C4 02                       push    ds:AllTextSelection_7
            .text:009F111C 8D 48 04                                lea     ecx, [eax+4]
            .text:009F111F 8D 45 E0                                lea     eax, [ebp-20h]
            .text:009F1122 50                                      push    eax
            .text:009F1123 E8 68 26 3F 00                          call    Ui__Text__String__toString
            .text:009F1128 8D 5F 06                                lea     ebx, [edi+6]
            .text:009F112B 8B 08                                   mov     ecx, [eax]
            .text:009F112D EB 1C                                   jmp     short loc_9F114B

            E8 ?? ?? ?? ?? 8B 44 87 08 83 CF FF
        */
        vector<PVOID> vCallSignedIndex = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\xE8\x00\x00\x00\x00\x8B\x44\x87\x08\x83\xCF\xFF", "x????xxxxxxx");
        if (vCallSignedIndex.size() != 1) {
            Logger.TraceWarn("Search SignedIndex falied.");
            return FALSE;
        }

        ULONG_PTR CallSignedIndex = (ULONG_PTR)vCallSignedIndex[0];
        g::fnSignedIndex = (fntIndex)(CallSignedIndex + 5 + *(INT*)(CallSignedIndex + 1));
    }

    {
        /*
            HistoryView__Message__updatePressed

            .text:009EE632                         loc_9EE632:                             ; CODE XREF: HistoryView__Message__updatePressed+100↑j
            .text:009EE632 8B CF                                   mov     ecx, edi
            .text:009EE634 E8 27 13 00 00                          call    HistoryView__Message__displayFromName
            .text:009EE639 8B CF                                   mov     ecx, edi
            .text:009EE63B E8 00 14 00 00                          call    HistoryView__Message__displayForwardedFrom
            .text:009EE640 84 C0                                   test    al, al
            .text:009EE642 74 0E                                   jz      short loc_9EE652
            .text:009EE644 8D 4C 24 18                             lea     ecx, [esp+18h]
            .text:009EE648 E8 A3 75 BE FF                          call    gsl__not_null_Calls__Call__Delegate_____operator__
            .text:009EE64D E8 AE DF EE FF                          call    RuntimeComponent_HistoryMessageForwarded_HistoryItem___Index
            .text:009EE652
            .text:009EE652                         loc_9EE652:                             ; CODE XREF: HistoryView__Message__updatePressed+122↑j

            // find this (RuntimeComponent<HistoryMessageReply,HistoryItem>::Index()
            .text:009EE652 E8 B9 52 FB FF                          call    RuntimeComponent_HistoryMessageReply_HistoryItem___Index

            .text:009EE657 8B 46 08                                mov     eax, [esi+8]
            .text:009EE65A 8B 38                                   mov     edi, [eax]
            .text:009EE65C E8 BF 53 FB FF                          call    RuntimeComponent_HistoryMessageVia_HistoryItem___Index
            .text:009EE661 8B 4C 87 08                             mov     ecx, [edi+eax*4+8]
            .text:009EE665 83 F9 04                                cmp     ecx, 4
            .text:009EE668 72 1D                                   jb      short loc_9EE687
            .text:009EE66A 8B 46 08                                mov     eax, [esi+8]
            .text:009EE66D 03 C1                                   add     eax, ecx
            .text:009EE66F 74 16                                   jz      short loc_9EE687
            .text:009EE671 8B 74 24 14                             mov     esi, [esp+14h]
            .text:009EE675 8B CE                                   mov     ecx, esi
            .text:009EE677 E8 E4 12 00 00                          call    HistoryView__Message__displayFromName
            .text:009EE67C 84 C0                                   test    al, al
            .text:009EE67E 75 07                                   jnz     short loc_9EE687
            .text:009EE680 8B CE                                   mov     ecx, esi
            .text:009EE682 E8 B9 13 00 00                          call    HistoryView__Message__displayForwardedFrom

            E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 46 08 8B 38
        */
        vector<PVOID> vResult = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\x46\x08\x8B\x38", "x????x????xxxxx");
        if (vResult.size() != 1) {
            Logger.TraceWarn("Search ReplyIndex falied.");
            return FALSE;
        }

        ULONG_PTR CallReplyIndex = (ULONG_PTR)vResult[0] + 5;
        g::fnReplyIndex = (fntIndex)(CallReplyIndex + 5 + *(INT*)(CallReplyIndex + 1));
    }
    
    {
        /*
            Lang::Instance *__cdecl Lang::Current()

            //////////////////////////////////////////////////

            2020.1.18 - 1.9.4

            Telegram.exe+6A74BB - 8B 0D 24A95B03        - mov ecx,[Telegram.exe+31FA924]
            Telegram.exe+6A74C1 - 03 C6                 - add eax,esi
            Telegram.exe+6A74C3 - 0FB7 C0               - movzx eax,ax
            Telegram.exe+6A74C6 - 85 C9                 - test ecx,ecx
            Telegram.exe+6A74C8 - 0F84 35010000         - je Telegram.exe+6A7603
            Telegram.exe+6A74CE - 8B 49 54              - mov ecx,[ecx+54]

            (byte)
            8B 0D ?? ?? ?? ?? 03 C6 0F B7 C0 85 C9 0F 84 ?? ?? ?? ?? 8B 49

            //////////////////////////////////////////////////

            2020.6.30 - 2.1.14 beta

            Telegram.exe+193CFC - 8B 0D 68687304        - mov ecx,[Telegram.exe+3C36868] { (04D45818) }
            Telegram.exe+193D02 - 03 C6                 - add eax,esi
            Telegram.exe+193D04 - 0FB7 C0               - movzx eax,ax
            Telegram.exe+193D07 - 85 C9                 - test ecx,ecx
            Telegram.exe+193D09 - 0F84 5F010000         - je Telegram.exe+193E6E
            Telegram.exe+193D0F - 8B 89 B4020000        - mov ecx,[ecx+000002B4]

            (uint32)
            8B 0D ?? ?? ?? ?? 03 C6 0F B7 C0 85 C9 0F 84 ?? ?? ?? ?? 8B
        */

        vector<PVOID> vCallCurrent;
        ULONG Offset;

        // ver < 2.1.14
        if (g::CurrentVersion < 2001014)
        {
            vCallCurrent = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x8B\x0D\x00\x00\x00\x00\x03\xC6\x0F\xB7\xC0\x85\xC9\x0F\x84\x00\x00\x00\x00\x8B\x49", "xx????xxxxxxxxx????xx");
            if (vCallCurrent.empty()) {
                Logger.TraceWarn("Search LangInstance falied.");
                return FALSE;
            }

            Offset = *(BYTE*)((ULONG_PTR)vCallCurrent[0] + 21);
        }
        // ver >= 2.1.14
        else if (g::CurrentVersion >= 2001014)
        {
            vCallCurrent = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x8B\x0D\x00\x00\x00\x00\x03\xC6\x0F\xB7\xC0\x85\xC9\x0F\x84\x00\x00\x00\x00\x8B", "xx????xxxxxxxxx????x");
            if (vCallCurrent.empty()) {
                Logger.TraceWarn("Search LangInstance falied.");
                return FALSE;
            }

            Offset = *(ULONG*)((ULONG_PTR)vCallCurrent[0] + 21);
        }

        g::ppLangInstance = (LanguageInstance**)(*(ULONG_PTR*)(*(ULONG_PTR*)((ULONG_PTR)vCallCurrent[0] + 2)) + Offset);
        if (g::ppLangInstance == NULL) {
            Logger.TraceWarn("LangInstance is null.");
            return FALSE;
        }
    }

    {
        /*
            Telegram.exe+724503 - 8B 49 20              - mov ecx,[ecx+20]
            Telegram.exe+724506 - 85 C9                 - test ecx,ecx
            Telegram.exe+724508 - 0F84 F2000000         - je Telegram.exe+724600
            Telegram.exe+72450E - 8B 01                 - mov eax,[ecx]
            Telegram.exe+724510 - FF 90 D8000000        - call dword ptr [eax+000000D8]
            Telegram.exe+724516 - 85 C0                 - test eax,eax

            8B 49 ?? 85 C9 0F 84 ?? ?? ?? ?? 8B 01 FF 90 ?? ?? ?? ?? 85 C0
        */
        vector<PVOID> vCallToHistoryMessage = Memory::FindPatternEx(GetCurrentProcess(), (PVOID)g::MainModule, MainModuleInfo.SizeOfImage, "\x8B\x49\x00\x85\xC9\x0F\x84\x00\x00\x00\x00\x8B\x01\xFF\x90\x00\x00\x00\x00\x85\xC0", "xx?xxxx????xxxx????xx");
        if (vCallToHistoryMessage.empty()) {
            Logger.TraceWarn("Search toHistoryMessage index falied. (1)");
            return FALSE;
        }

        ULONG Offset = *(ULONG*)((ULONG_PTR)vCallToHistoryMessage[0] + 15);

        if (Offset % sizeof(PVOID) != 0) {
            Logger.TraceWarn("Search toHistoryMessage index falied. (2)");
            return FALSE;
        }

        // Check each result
        for (PVOID Address : vCallToHistoryMessage)
        {
            if (*(ULONG*)((ULONG_PTR)Address + 15) != Offset) {
                Logger.TraceWarn("Search toHistoryMessage index falied. (3)");
                return FALSE;
            }
        }

        g::Offsets::Index_toHistoryMessage = (Offset / sizeof(PVOID)) - 1 /* Start from 0 */;
    }

    return TRUE;
}

BOOLEAN InitOffsets()
{
    // ver < 2.1.21
    if (g::CurrentVersion < 2001021) {
        return FALSE;
    }
    // ver >= 2.1.21, ver < 2.4
    else if (g::CurrentVersion >= 2001021 && g::CurrentVersion < 2004000) {
        g::Offsets::TimeText = 0x90;
        g::Offsets::TimeWidth = 0x94;
        g::Offsets::MainView = 0x5C;
        g::Offsets::Media = 0x54;
        g::Offsets::SignedTimeText = 0x14;
        g::Offsets::HistoryPeer = 0x7C;
    }
    // ver >= 2.4.0, ver < 2.4.1
    else if (g::CurrentVersion >= 2004000 && g::CurrentVersion < 2004001) {
        g::Offsets::TimeText = 0x70;          // changed
        g::Offsets::TimeWidth = 0x74;         // changed
        g::Offsets::MainView = 0x5C;
        g::Offsets::Media = 0x54;
        g::Offsets::SignedTimeText = 0x14;
        g::Offsets::HistoryPeer = 0x7C;
    }
    // ver >= 2.4.1
    else if (g::CurrentVersion >= 2004001) {
        g::Offsets::TimeText = 0x70;
        g::Offsets::TimeWidth = 0x74;
        g::Offsets::MainView = 0x5C;
        g::Offsets::Media = 0x54;
        g::Offsets::SignedTimeText = 0x10;    // changed
        g::Offsets::HistoryPeer = 0x7C;
    }

    return TRUE;
}

DWORD WINAPI Initialize(PVOID pParameter)
{
    auto &Logger = ILogger::GetInstance();

#ifdef _DEBUG
    MessageBoxW(NULL, L"Initialize", L"Anti-Revoke Plugin", MB_ICONINFORMATION);
#endif

    g::MainModule = (ULONG_PTR)GetModuleHandleW(L"Telegram.exe");
    g::CurrentVersion = File::GetCurrentVersion();

    if (g::MainModule == NULL || g::CurrentVersion == 0) {
        Logger.TraceError("Initialize failed.");
        return 0;
    }

    Updater::GetInstance().CheckUpdate();
    
    if (!InitOffsets()) {
        Logger.TraceError("You are using a version of Telegram that is deprecated by the plugin.\nPlease update your Telegram.", FALSE);
        return 0;
    }

    if (!SearchSigns()) {
        Logger.TraceError("SearchSigns() failed.");
        return 0;
    }

    MH_STATUS Status = MH_Initialize();
    if (Status != MH_OK) {
        Logger.TraceError(string("MH_Initialize() failed.\n") + MH_StatusToString(Status));
        return 0;
    }

    InitMarkLanguage();

    if (!HookMemoryFree(TRUE)) {
        Logger.TraceError("HookMemoryFree() failed.");
        return 0;
    }

    if (!HookRevoke(TRUE)) {
        Logger.TraceError("HookRevoke() failed.");
        return 0;
    }

    ProcessItems();

    return 0;
}

BOOLEAN CheckProcess()
{
    string CurrentName = File::GetCurrentName();
    if (Text::ToLower(CurrentName) != "telegram.exe") {
        ILogger::GetInstance().TraceWarn("This is not a Telegram process. [" + CurrentName + "]");
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI RealDllMain(HMODULE hModule, ULONG Reason, PVOID pReserved)
{
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Utils::CreateConsole();
        
        if (CheckProcess()) {
            CloseHandle(CreateThread(NULL, 0, Initialize, NULL, 0, NULL));
        }

        break;
    }

    return TRUE;
}
