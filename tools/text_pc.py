# The port's PC wording for the lines that talk about the Xbox console: the
# autosave warning, the disc error, free blocks, and the Dashboard (which on PC
# closes the game). Writes src/text_pc.inc for src/text_pc.c.
#
#   python tools/text_pc.py <game_files>/data_xb.rkv src/text_pc.inc
#
# In the texts below '|' is a line break (the game's two-character escape) and
# '@' is a button glyph, filled in from the line being replaced. Each line is
# checked against the game's English one: same glyph count, same format
# specifiers.
import re
import struct
import sys

LANGS = ['English', 'French', 'Spanish', 'German', 'Italian', 'Dutch']

PC = {
    'English': {
        'TEXT_XBOX_FATAL': "There's a problem with the game files. They may be missing or damaged. Reinstall them from the launcher.",
        'TEXT_MEMCARD_MANAGECARD': 'Quit to Desktop',
        'TEXT_MEMCARD_NOFREESPACE': "There isn't enough free disk space to save games.||You need to free %d more blocks (16 KB each).",
        'TEXT_MEMCARD_AUTOSAVEWARN2': "Please don't close the game or turn off your PC while the game is autosaving.||Press @ to continue.",
        'TEXT_MEMCARD_RESETWARN': 'Warning!||Quitting to the desktop will lose your game progress.||Continue?',
        'TEXT_MEMCARD_RESETWARN2': 'Warning!||Quitting to the desktop will lose your game data.||Continue?',
    },
    'French': {
        'TEXT_XBOX_FATAL': "Les fichiers du jeu présentent une anomalie. Ils sont peut-être absents ou endommagés. Réinstallez-les depuis le lanceur.",
        'TEXT_MEMCARD_MANAGECARD': 'Quitter vers le bureau',
        'TEXT_MEMCARD_NOFREESPACE': "Il n'y a pas assez d'espace disque libre pour sauvegarder les parties.||Vous avez besoin de %d blocs supplémentaires (16 Ko chacun).",
        'TEXT_MEMCARD_AUTOSAVEWARN2': 'Ne pas fermer le jeu ni éteindre le PC pendant la sauvegarde.||Appuyez sur @ pour continuer.',
        'TEXT_MEMCARD_RESETWARN': 'Attention !||Quitter vers le bureau va faire perdre votre progression.||Continuer ?',
        'TEXT_MEMCARD_RESETWARN2': 'Attention !||Quitter vers le bureau va faire perdre vos données de jeu.||Continuer ?',
    },
    'Spanish': {
        'TEXT_XBOX_FATAL': 'Hay un problema con los archivos del juego. Puede que falten o estén dañados. Vuelve a instalarlos desde el lanzador.',
        'TEXT_MEMCARD_MANAGECARD': 'Salir al escritorio',
        'TEXT_MEMCARD_NOFREESPACE': 'No hay suficiente espacio libre en el disco para guardar partidas.||Se necesitan %d bloques libres más (de 16 KB cada uno).',
        'TEXT_MEMCARD_AUTOSAVEWARN2': 'No cierres el juego ni apagues el PC mientras se guarda la partida automáticamente.||Pulsa @ para continuar.',
        'TEXT_MEMCARD_RESETWARN': '¡Atención!||Si sales al escritorio, perderás el progreso de la partida.||¿Continuar?',
        'TEXT_MEMCARD_RESETWARN2': '¡Atención!||Si sales al escritorio, perderás los datos de la partida.||¿Continuar?',
    },
    'German': {
        'TEXT_XBOX_FATAL': 'Mit den Spieldateien ist ein Problem aufgetreten. Möglicherweise fehlen sie oder sind beschädigt. Installiere sie über den Launcher neu.',
        'TEXT_MEMCARD_MANAGECARD': 'Zum Desktop',
        'TEXT_MEMCARD_NOFREESPACE': 'Auf der Festplatte ist nicht genug Platz frei, um Spiele zu speichern.||Du musst noch %d Blöcke (je 16 KB) freigeben.',
        'TEXT_MEMCARD_AUTOSAVEWARN2': 'Bitte das Spiel nicht beenden und den PC nicht ausschalten, während das Spiel automatisch speichert.||Weiter mit @',
        'TEXT_MEMCARD_RESETWARN': 'Warnung!||Durch Beenden zum Desktop geht dein Spielfortschritt verloren.||Fortfahren?',
        'TEXT_MEMCARD_RESETWARN2': 'Warnung!||Durch Beenden zum Desktop gehen deine Spieldaten verloren.||Fortfahren?',
    },
    'Italian': {
        'TEXT_XBOX_FATAL': 'Si è verificato un problema con i file di gioco. Potrebbero essere mancanti o danneggiati. Reinstallali dal launcher.',
        'TEXT_MEMCARD_MANAGECARD': 'Esci al desktop',
        'TEXT_MEMCARD_NOFREESPACE': "Non c'è abbastanza spazio libero sul disco per salvare le partite.||Devi liberare altri %d blocchi (da 16 KB ciascuno).",
        'TEXT_MEMCARD_AUTOSAVEWARN2': 'Non chiudere il gioco e non spegnere il PC durante il salvataggio automatico.||Premi @ per continuare.',
        'TEXT_MEMCARD_RESETWARN': 'Attenzione!||Uscendo al desktop perderai i progressi di gioco.||Continuare?',
        'TEXT_MEMCARD_RESETWARN2': 'Attenzione!||Uscendo al desktop perderai i dati di gioco.||Continuare?',
    },
    'Dutch': {
        'TEXT_XBOX_FATAL': 'Er is een probleem met de gamebestanden. Misschien ontbreken ze of zijn ze beschadigd. Installeer ze opnieuw via de launcher.',
        'TEXT_MEMCARD_MANAGECARD': 'Naar bureaublad',
        'TEXT_MEMCARD_NOFREESPACE': 'Er is onvoldoende vrije schijfruimte om games op te slaan.||Je hebt nog %d vrije blokken (van 16 KB) nodig.',
        'TEXT_MEMCARD_AUTOSAVEWARN2': 'Sluit de game niet af en zet de pc niet uit terwijl de game automatisch wordt opgeslagen.||Druk op @ om door te gaan.',
        'TEXT_MEMCARD_RESETWARN': 'Waarschuwing!||Als je naar het bureaublad gaat, gaat je voortgang in de game verloren.||Doorgaan?',
        'TEXT_MEMCARD_RESETWARN2': 'Waarschuwing!||Als je naar het bureaublad gaat, gaan je gamegegevens verloren.||Doorgaan?',
    },
}

BS = chr(92)
GLYPH = re.compile('<font=font_buttons:0>.*?</font>')
SPEC = re.compile('%[-+ #0]*[0-9]*(?:[.][0-9]+)?[a-zA-Z]')


def english_lines(rkv):
    f = open(rkv, 'rb')
    head = f.read(0x80)
    n = struct.unpack_from('<I', head, 4)[0]
    ent = f.read(n * 20)
    names = f.read(1 << 20)
    for i in range(n):
        no, _, size, off, _ = struct.unpack_from('<5I', ent, i * 20)
        name = names[no:names.index(b'\0', no)].decode('latin1')
        if name.lower() == 'translations.english.txt':
            f.seek(off)
            text = f.read(size).decode('cp1252')
            out = {}
            for line in text.split('\r\n'):
                if '\t' in line:
                    k, v = line.split('\t', 1)
                    out[k.split('=')[0]] = v
            return out
    raise SystemExit('translations.English.txt not found')


def check(en):
    bad = 0
    for lang, lines in PC.items():
        assert set(lines) == set(PC['English']), lang
        for key, text in lines.items():
            ref = en[key]
            if text.count('@') != len(GLYPH.findall(ref)):
                print('glyphs differ:', lang, key); bad += 1
            if SPEC.findall(text.replace('|', ' ')) != SPEC.findall(ref):
                print('format differs:', lang, key); bad += 1
            text.encode('cp1252')
    if bad:
        raise SystemExit(1)


def c_string(s):
    out = '"'
    for b in s.replace('|', BS + 'n').encode('cp1252'):
        if b == 0x22 or b == 0x5C or b < 0x20 or b > 0x7E:
            out += BS + '%03o' % b
        else:
            out += chr(b)
    return out + '"'


def main():
    en = english_lines(sys.argv[1])
    check(en)
    with open(sys.argv[2], 'w', newline='\r\n') as f:
        f.write('/* Generated by tools/text_pc.py. { language, key, text }: language as\n')
        f.write(' * in text_pc.h, text in cp1252 with the game escapes; an at sign is a\n')
        f.write(' * button glyph taken from the line being replaced. */\n')
        for li, lang in enumerate(LANGS):
            for key, text in PC[lang].items():
                f.write('{ %d, "%s", %s },\n' % (li, key, c_string(text)))
    print('wrote', sum(len(v) for v in PC.values()), 'lines')


main()
