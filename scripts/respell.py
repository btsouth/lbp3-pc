"""Import names in Bloodborne's spelling, for other games' eboots.

An import is NID#L#M, where L and M index the eboot's own library and module tables, so the
same function is Ou3iL1abvng#p#J in Bloodborne and Ou3iL1abvng#r#e in Ratchet & Clank. The
host runtime's tables use Bloodborne's spelling; this maps a name by library and module name.
A library Bloodborne does not import becomes NID#libraryName, as link_modules spells them.
"""

# Bloodborne (CUSA03173 / CUSA00900) eboot tables: name -> letter.
LIBRARIES = {
    'libScePad': 'A', 'libSceGnmDriver': 'B', 'libSceVideoOut': 'C', 'libSceUserService': 'D',
    'libSceNet': 'E', 'libSceFios2': 'F', 'libSceAudioOut': 'G', 'libSceRtc': 'H',
    'libScePosix': 'I', 'libSceNetCtl': 'J', 'libSceHttp': 'K', 'libSceSsl': 'L',
    'libSceSysmodule': 'M', 'libSceAjm': 'N', 'libSceSaveData': 'O', 'libSceNpMatching2': 'P',
    'libSceNpCommon': 'Q', 'libSceNpManager': 'R', 'libSceNpUtility': 'S', 'libSceNpSignaling': 'T',
    'libSceMouse': 'U', 'libSceSystemService': 'V', 'libSceNpWebApi': 'W', 'libSceNpAuth': 'X',
    'libSceAudioIn': 'Y', 'libSceCommonDialog': 'Z', 'libSceWebBrowserDialog': 'a',
    'libSceImeDialog': 'b', 'libSceAppContent': 'c', 'libSceSaveDataDialog': 'd',
    'libSceGameLiveStreaming': 'e', 'libSceVideoRecording': 'f', 'libSceScreenShot': 'g',
    'libSceNpProfileDialog': 'h', 'libSceVoice': 'i', 'libSceMsgDialog': 'j', 'libSceNpCommerce': 'k',
    'libSceNpTrophy': 'l', 'libSceAvPlayer': 'm', 'libScePlayGo': 'n', 'libSceNpScore': 'o',
    'libkernel': 'p', 'libc': 'q',
}
MODULES = {
    'libScePad': 'B', 'libSceGnmDriver': 'C', 'libSceVideoOut': 'D', 'libSceUserService': 'E',
    'libSceNet': 'F', 'libSceFios2': 'G', 'libSceAudioOut': 'H', 'libSceRtc': 'I', 'libkernel': 'J',
    'libSceNetCtl': 'K', 'libSceHttp': 'L', 'libSceSsl': 'M', 'libSceSysmodule': 'N', 'libSceAjm': 'O',
    'libSceSaveData': 'P', 'libSceNpMatching2': 'Q', 'libSceNpCommon': 'R', 'libSceNpManager': 'S',
    'libSceNpUtility': 'T', 'libSceNpSignaling': 'U', 'libSceMouse': 'V', 'libSceSystemService': 'W',
    'libSceNpWebApi': 'X', 'libSceNpAuth': 'Y', 'libSceAudioIn': 'Z', 'libSceCommonDialog': 'a',
    'libSceWebBrowserDialog': 'b', 'libSceImeDialog': 'c', 'libSceAppContentUtil': 'd',
    'libSceSaveDataDialog': 'e', 'libSceGameLiveStreaming': 'f', 'libSceVideoRecording': 'g',
    'libSceScreenShot': 'h', 'libSceNpProfileDialog': 'i', 'libSceVoice': 'j', 'libSceMsgDialog': 'k',
    'libSceNpCommerce': 'l', 'libSceNpTrophy': 'm', 'libSceAvPlayer': 'n', 'libScePlayGo': 'o',
    'libSceNpScore': 'p', 'libc': 'q',
}


def respell(name, libraries, modules):
    """name in the eboot's spelling -> Bloodborne's; libraries/modules: letter -> (name, version)."""
    parts = name.split('#')
    if len(parts) != 3 or parts[1] not in libraries or parts[2] not in modules:
        return name
    nid, lib, mod = parts[0], libraries[parts[1]][0], modules[parts[2]][0]
    if lib in LIBRARIES and mod in MODULES:
        return f'{nid}#{LIBRARIES[lib]}#{MODULES[mod]}'
    return f'{nid}#{lib}'
