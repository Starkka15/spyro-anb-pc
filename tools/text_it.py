"""Italian for the Xbox lines the Italian translation left in English.

Written for this port from the Xbox English, in the terms the European
release's own Italian uses (Colpo di coda, Tripla sferzata con la coda,
Abilita salvataggio automatico, ...). '@' stands for each button glyph, filled
in order from the Xbox English line, so the Xbox's own buttons show. The rest
of the missing lines come from the European PS2 disc at import time.

tools/text_it.py emits src/launcher/text_it.inc (cp1252 as octal escapes).
"""
import os, re

IT = {
    "TEXT_STICK": "Levetta sinistra",
    "TEXT_XBOX_FATAL": "Si è verificato un problema con il disco in uso. Il disco potrebbe essere sporco o danneggiato.",
    "TEXT_DIALOG_ONE": "Usa la levetta sinistra @ per spostare Spyro.",
    "TEXT_DIALOG_FIVE": "Premi @,@ e poi tieni premuto @ per eseguire un Colpo di coda.",
    "TEXT_DIALOG_SIX": "Tieni premuto @ per caricare.",
    "TEXT_MENU_CONTROLLERREMOVED": "Controller scollegato.%sRicollegare il controller alla porta controller %d e premere START@ per continuare.",
    "TEXT_MEMCARD_SLOT2": "disco rigido",
    "TEXT_MEMCARD_MANAGECARD": "Dashboard Xbox",
    "TEXT_MEMCARD_FULLCARD": "Disco rigido pieno",
    "TEXT_MEMCARD_CORRUPT": "Danneggiato",
    "TEXT_MEMCARD_SELECTCARD": "Selezionare l'unità di memoria.",
    "TEXT_MEMCARD_SELECTFILE": "Selezionare un salvataggio.",
    "TEXT_MEMCARD_NOCARDFOUND": "Nessuna unità di memoria trovata.\\n\\nInserire un'unità di memoria.",
    "TEXT_MEMCARD_NOCARDBOOT": "Nessuna unità di memoria trovata.\\n\\nInserire un'unità di memoria con almeno %d blocchi liberi o che contenga dati di gioco salvati di %t.",
    "TEXT_MEMCARD_NOTFORMATTED": "Il dispositivo %s non è formattato.",
    "TEXT_MEMCARD_NOSAVEGAME": "Il dispositivo %s non contiene partite salvate di %t.",
    "TEXT_MEMCARD_CORRUPTDATA": "Il dispositivo %s contiene dati danneggiati.",
    "TEXT_MEMCARD_NOFREESPACE": "Blocchi liberi insufficienti su Xbox per salvare le partite.\\n\\nÈ necessario liberare altri %d blocchi.",
    "TEXT_MEMCARD_SAVEGAMESPACE": "Spazio libero insufficiente sul dispositivo %s per salvare la partita.\\n\\nInserire un'unità di memoria con almeno %d blocchi liberi o che contenga dati di gioco salvati di %t.",
    "TEXT_MEMCARD_SAVEGAMESPACE2": "Spazio libero insufficiente sul dispositivo %s per salvare la partita.\\n\\nSono necessari almeno %d blocchi liberi per salvare i dati di gioco di %t.\\n\\nAvviare comunque il gioco?",
    "TEXT_MEMCARD_NODATA": "Il dispositivo %s non contiene dati di gioco salvati di %t.\\n\\nCreare un nuovo salvataggio?",
    "TEXT_MEMCARD_FORMATCARD": "Il dispositivo %s non è formattato.\\n\\nFormattarlo ora?",
    "TEXT_MEMCARD_READING": "Caricamento della partita salvata in corso...\\n\\nNon rimuovere le unità di memoria e non scollegare i controller.",
    "TEXT_MEMCARD_WRITING": "Salvataggio su %s in corso...\\n\\nNon rimuovere le unità di memoria e non scollegare i controller.",
    "TEXT_MEMCARD_FORMATTING": "Formattazione di %s in corso...\\n\\nNon rimuovere le unità di memoria e non scollegare i controller.",
    "TEXT_MEMCARD_READFAIL": "Impossibile caricare la partita salvata.\\n\\nPremere @ per continuare.",
    "TEXT_MEMCARD_WRITEFAIL": "Salvataggio fallito!",
    "TEXT_MEMCARD_FORMATFAIL": "Formattazione fallita!",
    "TEXT_MEMCARD_AUTOSAVEERROR": "Salvataggio fallito!\\n\\nL'Autosalvataggio è stato disattivato.\\n\\nPer riattivarlo, salvare manualmente il gioco con l'opzione \"Abilita salvataggio automatico\" nel menu Pausa.\\n\\nPremere @ per continuare.",
    "TEXT_MEMCARD_CARDREMOVED": "Il dispositivo %s è stato rimosso.\\n\\nL'Autosalvataggio è stato disattivato.\\n\\nPer riattivarlo, salvare manualmente il gioco con l'opzione \"Abilita salvataggio automatico\" nel menu Pausa.\\n\\nPremere @ per continuare.",
    "TEXT_MEMCARD_AUTOSAVEWARN2": "Non spegnere la console Xbox durante l'Autosalvataggio.\\n\\nPremere @ per continuare.",
    "TEXT_MEMCARD_DASHBOARD1": "Premi @ per continuare senza salvare oppure",
    "TEXT_MEMCARD_DASHBOARD2": "premi @ per liberare altri blocchi.",
    "TEXT_MEMCARD_RESETWARN": "Attenzione!\\n\\nAvviando la Dashboard Xbox i progressi di gioco andranno perduti.\\n\\nContinuare?",
    "TEXT_MEMCARD_RESETWARN2": "Attenzione!\\n\\nAvviando la Dashboard Xbox i dati di gioco andranno perduti.\\n\\nContinuare?",
    "TEXT_MENU_PRESSSTART": "Premi START",
    "TEXT_MENU_RUMBLE": "Vibrazione",
    "TEXT_DIALOG_FIFTEEN": "Usa la levetta destra @ per spostare la telecamera e trovare Sparx.",
    "TEXT_DIALOG_SIXTEEN": "Premi @,@,@,@ per eseguire una Tripla sferzata con la coda e rompere il cranio del serpente.",
    "TEXT_DIALOG_TWENTYNINE": "Puoi spostarti lateralmente intorno ai nemici tenendo premuto @",
    "TEXT_DIALOG_THIRTYTHREE": "Premi @ per accelerare e @ per rallentare.",
    "TEXT_DIALOG_FORTY": "Premi @ mentre tieni la levetta sinistra @ verso sinistra o destra per rotolare lontano dal pericolo.",
    "TEXT_DIALOG_FORTYTHREE": "Usa la levetta sinistra @ per cambiare la velocità e per inclinare il carrello da miniera.",
    "DOJO_F_KU_2": "Premi senza rilasciare @ per lanciare in aria il manichino, quindi premi @,@,@,@ per colpirlo tre volte con un assalto aereo.",
    "DOJO_F_KU_3": "Premi @ per saltare, quindi nel punto più alto del salto premi @ per colpire ciascun manichino con una cornata in tuffo.",
    "DOJO_F_KU_4": "Premi @,@,@,@ per assaltare il manichino tre volte e lanciarlo in aria, quindi premi @,@,@,@ per saltare in alto e colpirlo tre volte con un assalto aereo.",
    "DOJO_F_KB_1": "Premi senza rilasciare @ per lanciare in aria il manichino, quindi premi @,@ per raggiungerlo con un salto e spazzarlo via.",
    "DOJO_F_KB_2": "Premi @,@,@,@ per assaltare il manichino tre volte e lanciarlo in aria, quindi premi @,@ per raggiungerlo con un salto e spazzarlo via.",
    "DOJO_F_KB_3": "Premi senza rilasciare @ per lanciare in aria il manichino, quindi premi @,@ per scagliarlo contro l'altro manichino. Ripeti l'azione cinque volte.",
    "DOJO_F_LR_2": "Resta dietro la parete elementale e premi @,@,@ per eseguire un doppio salto e lanciare più lontano una Sfera di Fuoco.",
    "DOJO_E_SR_1": "Premi senza rilasciare @ per stordire i manichini vicini ai bersagli. Usando la levetta sinistra @, lancia i nemici contro gli oggetti circostanti per frantumarli.",
}

TAG = re.compile(r"<font=font_buttons:0>.*?</font>")
FMT = re.compile(r"%[-.0-9]*[a-z]")


def check(english):
    """english: {key: Xbox English}. Raises on a glyph or format mismatch."""
    for k, v in IT.items():
        en = english[k]
        if v.count("@") != len(TAG.findall(en)):
            raise SystemExit("%s: %d glyphs, English has %d" % (k, v.count("@"), len(TAG.findall(en))))
        if FMT.findall(v) != FMT.findall(TAG.sub("", en)):
            raise SystemExit("%s: format %r vs %r" % (k, FMT.findall(v), FMT.findall(TAG.sub("", en))))
        v.encode("cp1252")


def c_string(s):
    out = []
    for b in s.encode("cp1252"):
        if b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:       # path to the Xbox translations.English.txt, to check against
        en = {}
        for ln in open(sys.argv[1], "rb").read().decode("cp1252").split("\r\n"):
            if ln:
                k, _, v = ln.partition("\t")
                en[k.split("=")[0]] = v
        check(en)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "src", "launcher", "text_it.inc"), "w", newline="\n") as f:
        f.write("/* Generated by tools/text_it.py: Italian for the Xbox lines the Italian\n"
                " * translation left in English. '@' = the English line's next button glyph. */\n")
        for k, v in IT.items():
            f.write("{ \"%s\", %s },\n" % (k, c_string(v)))
    print("%d lines" % len(IT))
