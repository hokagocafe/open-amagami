#include "ui_text.hpp"

#include <windows.h>

#include <unordered_map>

namespace amg::ui {

namespace {

struct Row {
    const char *en, *es, *ja;
};

// One row per text: English (the key), Spanish, Japanese. `python tools/ui_keys.py` checks that every text the code asks for
// is here and that the values ({0}, {1}) are in every language.
const Row ROWS[] = {
    // --- the launcher -----------------------------------------------------------------------------------------------
    {"Start game", "Jugar", "ゲームを始める"},
    {"Choose game folder...", "Elegir la carpeta del juego...", "ゲームのフォルダーを選ぶ..."},
    {"Settings", "Ajustes", "設定"},
    {"Close", "Cerrar", "閉じる"},
    {"Save", "Guardar", "保存"},
    {"Cancel", "Cancelar", "キャンセル"},
    {"Download and install {0} now? The program restarts when it is done.", "¿Descargar e instalar {0} ahora? El programa se reinicia al terminar.",
     "{0} をダウンロードしてインストールしますか? 終わるとプログラムが再起動します。"},
    {"Downloading...", "Descargando...", "ダウンロード中..."},
    {"Checking...", "Comprobando...", "確認中..."},
    {"Installing...", "Instalando...", "インストール中..."},
    {"Open the download page instead?", "¿Abrir la página de descarga en su lugar?", "代わりにダウンロードページを開きますか?"},
    {"The update is installed. Start the program again to use it.", "La actualización está instalada. Abre el programa otra vez para usarla.",
     "アップデートをインストールしました。プログラムをもう一度起動してください。"},
    {"The program's folder is not known.", "No se conoce la carpeta del programa.", "プログラムのフォルダーが分かりません。"},
    {"The update could not be downloaded ({0}).", "No se pudo descargar la actualización ({0}).", "アップデートをダウンロードできませんでした ({0})。"},
    {"The update could not be read ({0}).", "No se pudo leer la actualización ({0}).", "アップデートを読み取れませんでした ({0})。"},
    {"The download did not match its checksum, so it was not installed.", "La descarga no coincidió con su suma de verificación, así que no se instaló.",
     "ダウンロードがチェックサムと一致しなかったため、インストールしませんでした。"},
    {"The download does not hold the program, so it was not installed.", "La descarga no contiene el programa, así que no se instaló.",
     "ダウンロードにプログラムが含まれていないため、インストールしませんでした。"},
    {"The update could not be written to the program's folder (is it in a protected place?).",
     "No se pudo escribir la actualización en la carpeta del programa (¿está en un lugar protegido?).",
     "アップデートをプログラムのフォルダーに書き込めませんでした (保護された場所では?)。"},
    {"The update could not be installed, and the program was left as it was.", "No se pudo instalar la actualización y el programa quedó como estaba.",
     "アップデートをインストールできず、プログラムは元のままです。"},
    {"Choose your own decrypted PS Vita copy, or drop its folder here.",
     "Elige tu propia copia descifrada de PS Vita, o suelta su carpeta aquí.",
     "復号したPS Vita版のフォルダーを選ぶか、ここにドロップしてください。"},
    {"Bicubic", "Bicúbico", "バイキュービック"},
    {"Brightness", "Brillo", "明るさ"},
    {"Fit, blurred edges", "Ajustar, bordes difuminados", "収める(ぼかし)"},
    {"Frame counter", "Contador de fotogramas", "フレームカウンター"},
    {"Full screen", "Pantalla completa", "全画面"},
    {"Gamma", "Gamma", "ガンマ"},
    {"Lanczos", "Lanczos", "ランチョス"},
    {"Sharpen", "Nitidez", "鮮明"},
    {"Vertical sync", "Sincronía vertical", "垂直同期"},
    {"Window mode", "Modo de ventana", "ウィンドウ表示"},
    {"Windowed", "En ventana", "ウィンドウ"},
    {"Your game is ready. In the game, F8 opens the options.", "Tu juego está listo. En el juego, F8 abre las opciones.",
     "準備ができました。ゲーム中は F8 でオプションを開けます。"},
    {"Use the copy found in your Vita3K", "Usar la copia encontrada en tu Vita3K", "Vita3Kで見つかったコピーを使う"},
    {"Use the copy found in {0}", "Usar la copia encontrada en {0}", "{0} で見つかったコピーを使う"},
    {"Checking your game folder...", "Revisando la carpeta de tu juego...", "ゲームのフォルダーを確認しています..."},
    {"Choose the folder of your own copy of the game to begin.", "Elige la carpeta de tu propia copia del juego para empezar.",
     "始めるには、お持ちのゲームのコピーのフォルダーを選んでください。"},
    {"Get the new version, {0}", "Obtener la versión nueva, {0}", "新しいバージョンを入手: {0}"},
    {"Game files", "Archivos del juego", "ゲームファイル"},
    {"General", "General", "全般"},
    {"Display", "Pantalla", "画面"},
    {"Folder: {0}", "Carpeta: {0}", "フォルダー: {0}"},
    {"No folder chosen yet.", "Todavía no se eligió una carpeta.", "フォルダーがまだ選ばれていません。"},
    {"Every key and every pad button of the game.", "Cada tecla y cada botón del mando para el juego.", "ゲームのすべてのキーとパッドのボタン。"},
    {"For a pad the program does not recognise.", "Para un mando que el programa no reconoce.", "プログラムが認識しないパッド用。"},
    // --- the checks of the game folder -----------------------------------------------------------------------------
    {"Game folder", "Carpeta del juego", "ゲームフォルダー"},
    {"Game data", "Datos del juego", "ゲームデータ"},
    {"Decrypted copy", "Copia descifrada", "復号済みのコピー"},
    {"Executable", "Ejecutable", "実行ファイル"},
    {"Sound", "Sonido", "サウンド"},
    {"Version", "Versión", "バージョン"},
    {"Language", "Idioma", "言語"},
    {"AMAGAMI.IMG, SCENARIO.CPK, graph and the executable are there ({0} checked).",
     "AMAGAMI.IMG, SCENARIO.CPK, graph y el ejecutable están ({0} revisados).",
     "AMAGAMI.IMG、SCENARIO.CPK、graph、実行ファイルがあります({0}を確認)。"},
    {"The archives and the executable are readable.", "Los archivos y el ejecutable se pueden leer.", "アーカイブと実行ファイルを読み込めます。"},
    {"This is the executable of the English fan patch, which the port cannot use. Use the original game's eboot.bin (from the unpatched game) in its place; the translated data files can stay.",
     "Este es el ejecutable del parche de fans en inglés, que el port no puede usar. Pon en su lugar el eboot.bin del juego original (del juego sin parche); los archivos de datos traducidos pueden quedarse.",
     "これは英語ファンパッチの実行ファイルで、このポートでは使えません。代わりに元のゲーム(パッチなし)のeboot.binを使ってください。翻訳済みのデータファイルはそのままで構いません。"},
    {"Voices, music and sound effects are there.", "Están las voces, la música y los efectos de sonido.", "音声、音楽、効果音があります。"},
    {"voices", "voces", "音声"},
    {"music", "música", "音楽"},
    {"ambience", "ambiente", "環境音"},
    {"sound effects (bank {0})", "efectos de sonido (banco {0})", "効果音(バンク{0})"},
    {"The table of known versions is not there, so the files were not compared.", "No está la tabla de versiones conocidas, así que no se compararon los archivos.",
     "既知のバージョンの表がないため、ファイルは比較されていません。"},
    {"Known files (PCSG00291).", "Archivos conocidos (PCSG00291).", "既知のファイルです(PCSG00291)。"},
    {"{0} of 3 key files are not in the table of known versions (a patched or different copy). It may still work.",
     "{0} de 3 archivos clave no están en la tabla de versiones conocidas (una copia con parche o distinta). Puede que aun así funcione.",
     "主要な3つのファイルのうち{0}つが既知のバージョンの表にありません(パッチ適用済みか別のコピー)。それでも動く場合があります。"},
    {"English: this copy has the fan translation (v1.1) built in.", "Inglés: esta copia ya trae la traducción de fans (v1.1).",
     "英語: このコピーにはファン翻訳(v1.1)が組み込まれています。"},
    {"Japanese, with the English fan translation added: choose it in Game files, Language.",
     "Japonés, con la traducción de fans al inglés añadida: elígela en Archivos del juego, Idioma.", "日本語。英語ファン翻訳が追加済みです。「ゲームファイル」の「言語」で選べます。"},
    {"Japanese. To play in English, add the fan translation in Game files, Add English patch.",
     "Japonés. Para jugar en inglés, añade la traducción de fans en Archivos del juego, Parche en inglés.",
     "日本語。英語で遊ぶには、「ゲームファイル」の「英語パッチを追加...」でファン翻訳を追加してください。"},
    {"That folder does not exist.", "Esa carpeta no existe.", "そのフォルダーは存在しません。"},
    {"This does not look like the game's folder for the PS Vita version (PCSG00291). Missing:{0}\n\nChoose the folder that holds AMAGAMI.IMG, SCENARIO.CPK, graph and sound.",
     "Esta no parece la carpeta del juego de la versión de PS Vita (PCSG00291). Falta:{0}\n\nElige la carpeta que tiene AMAGAMI.IMG, SCENARIO.CPK, graph y sound.",
     "PS Vita版(PCSG00291)のゲームフォルダーではないようです。不足:{0}\n\nAMAGAMI.IMG、SCENARIO.CPK、graph、soundがあるフォルダーを選んでください。"},
    {"{0} is not readable: it is still encrypted by the console's file system. This program does not decrypt games; use a copy whose files have been decrypted (a dump made with the usual homebrew tools).",
     "{0} no se puede leer: sigue cifrado por el sistema de archivos de la consola. Este programa no descifra juegos; usa una copia con los archivos ya descifrados (un volcado hecho con las herramientas homebrew habituales).",
     "{0}を読み込めません。本体のファイルシステムによる暗号化が残っています。このプログラムはゲームを復号しません。復号済みのコピー(一般的なホームブリューツールで作ったダンプ)を使ってください。"},
    // --- the checks of the game folder: what is wrong and what to do (docs/technical/port.md, "No dead ends") -------------------
    {"Intact files", "Archivos íntegros", "ファイルの完全性"},
    {"No folder chosen yet. Click \"Choose game folder...\" or drop the game's folder on this window.",
     "Aún no se eligió ninguna carpeta. Pulsa «Elegir la carpeta del juego...» o suelta aquí la carpeta del juego.",
     "フォルダーがまだ選ばれていません。「ゲームのフォルダーを選ぶ...」を押すか、ここにフォルダーをドロップしてください。"},
    {"The folder {0} is not there any more. It may have been moved, renamed or deleted, or its drive is not connected. Choose the folder again.",
     "La carpeta {0} ya no está. Puede que se haya movido, renombrado o borrado, o que su unidad no esté conectada. Elige la carpeta otra vez.",
     "フォルダー {0} が見つかりません。移動・名前変更・削除されたか、ドライブが未接続の可能性があります。もう一度選んでください。"},
    {"{0} is a file, not a folder. Choose the folder that holds the game's files.",
     "{0} es un archivo, no una carpeta. Elige la carpeta que tiene los archivos del juego.",
     "{0} はフォルダーではなくファイルです。ゲームのファイルがあるフォルダーを選んでください。"},
    {"Missing: {0}. Choose the folder that has these files (the PS Vita version, PCSG00291), or copy the missing ones into this folder.",
     "Falta: {0}. Elige la carpeta que tiene estos archivos (la versión de PS Vita, PCSG00291) o copia los que faltan en esta carpeta.",
     "不足: {0}。これらのファイルがあるフォルダー(PS Vita版、PCSG00291)を選ぶか、不足分をこのフォルダーにコピーしてください。"},
    {"This folder is empty. Choose the folder that holds the game's files (AMAGAMI.IMG, SCENARIO.CPK, graph, eboot.bin).",
     "Esta carpeta está vacía. Elige la carpeta que tiene los archivos del juego (AMAGAMI.IMG, SCENARIO.CPK, graph, eboot.bin).",
     "このフォルダーは空です。ゲームのファイル(AMAGAMI.IMG、SCENARIO.CPK、graph、eboot.bin)があるフォルダーを選んでください。"},
    {"The game's files are in a folder inside this one: {0}. Choose that folder.",
     "Los archivos del juego están en una carpeta dentro de esta: {0}. Elige esa carpeta.",
     "ゲームのファイルはこの中のフォルダーにあります: {0}。そのフォルダーを選んでください。"},
    {"The path of this folder is very long ({0} characters) and Windows may refuse to open it. Move the game's folder to a shorter place, such as C:\\Amagami, and choose it again.",
     "La ruta de esta carpeta es muy larga ({0} caracteres) y Windows puede negarse a abrirla. Mueve la carpeta del juego a un lugar más corto, como C:\\Amagami, y elígela otra vez.",
     "このフォルダーのパスはとても長く({0}文字)、Windowsが開けない場合があります。ゲームのフォルダーを C:\\Amagami のような短い場所に移して、選び直してください。"},
    {"This folder could not be checked ({0}). Choose another folder, or check that you can read it.",
     "No se pudo revisar esta carpeta ({0}). Elige otra carpeta o comprueba que puedes leerla.",
     "このフォルダーを確認できませんでした({0})。別のフォルダーを選ぶか、読み取れるか確かめてください。"},
    {"{0} is damaged: it cannot be read as the game's data. Copy the game's files again from your original and choose the folder again.",
     "{0} está dañado: no se puede leer como datos del juego. Copia otra vez los archivos del juego desde tu original y elige la carpeta de nuevo.",
     "{0} が壊れています。ゲームのデータとして読み込めません。元のコピーからファイルをコピーし直して、フォルダーを選び直してください。"},
    {"{0} is incomplete: it is {1} but the game needs at least {2}. The copy was cut short. Copy the game's files again and choose the folder again.",
     "{0} está incompleto: pesa {1} y el juego necesita al menos {2}. La copia quedó a medias. Copia otra vez los archivos del juego y elige la carpeta de nuevo.",
     "{0} が不完全です。サイズは {1} ですが、{2} 以上が必要です。コピーが途中で終わっています。ファイルをコピーし直して、フォルダーを選び直してください。"},
    {"{0} is empty. Copy the game's files again from your original and choose the folder again.",
     "{0} está vacío. Copia otra vez los archivos del juego desde tu original y elige la carpeta de nuevo.",
     "{0} が空です。元のコピーからファイルをコピーし直して、フォルダーを選び直してください。"},
    {"{0} cannot be opened. Is another program using it, or is access to it denied? Close the other program, or copy the game's files to a folder of your own and choose that one.",
     "{0} no se puede abrir. ¿Lo usa otro programa o se niega el acceso? Cierra el otro programa, o copia los archivos del juego a una carpeta tuya y elige esa.",
     "{0} を開けません。他のプログラムが使用中か、アクセスが拒否されていませんか? 他のプログラムを閉じるか、ファイルを自分のフォルダーにコピーして、そのフォルダーを選んでください。"},
    {"The game's executable (eboot.bin) is still encrypted. This program does not decrypt games: use a copy whose executable has been decrypted.",
     "El ejecutable del juego (eboot.bin) sigue cifrado. Este programa no descifra juegos: usa una copia cuyo ejecutable ya esté descifrado.",
     "ゲームの実行ファイル(eboot.bin)が暗号化されたままです。このプログラムは復号しません。実行ファイルが復号済みのコピーを使ってください。"},
    {"The game's executable (eboot.bin) is damaged, or it is not the game's. Copy it again from your decrypted copy.",
     "El ejecutable del juego (eboot.bin) está dañado o no es el del juego. Cópialo otra vez desde tu copia descifrada.",
     "ゲームの実行ファイル(eboot.bin)が壊れているか、このゲームのものではありません。復号済みのコピーからコピーし直してください。"},
    {"Missing readable sources: {0}. The game will run with parts of the sound missing. Copy the game's sound folder next to the other files to fix it.",
     "Faltan fuentes legibles: {0}. El juego funcionará sin parte del sonido. Para arreglarlo, copia la carpeta sound del juego junto a los demás archivos.",
     "読み込めないもの: {0}。一部の音が出ないままゲームは動きます。直すには、soundフォルダーを他のファイルの隣にコピーしてください。"},
    // --- when the program stops by itself, or the game cannot start ---------------------------------------------------------
    {"The program stopped unexpectedly.", "El programa se detuvo de forma inesperada.", "プログラムが予期せず停止しました。"},
    {"It stopped right at the start, so the copy of the game may be damaged. The launcher opens the next time you start the program: choose the game's folder again, or copy the game's files again.",
     "Se detuvo justo al empezar, así que la copia del juego puede estar dañada. La próxima vez que abras el programa se abrirá el lanzador: elige otra vez la carpeta del juego, o copia de nuevo los archivos del juego.",
     "起動直後に停止したため、ゲームのコピーが壊れているかもしれません。次回の起動ではランチャーが開きます。フォルダーを選び直すか、ファイルをコピーし直してください。"},
    {"A report of what happened was saved in:\n{0}\n\nOpen that folder?",
     "Se guardó un informe de lo ocurrido en:\n{0}\n\n¿Abrir esa carpeta?",
     "状況の報告を次の場所に保存しました:\n{0}\n\nそのフォルダーを開きますか?"},
    {"A report could not be saved: the program's data folder cannot be written.",
     "No se pudo guardar un informe: no se puede escribir en la carpeta de datos del programa.",
     "報告を保存できませんでした。プログラムのデータフォルダーに書き込めません。"},
    {"The game could not start:\n{0}\n\nThe copy of the game may be damaged or incomplete. The launcher opens the next time you start the program: choose the game's folder again, or copy the game's files again.",
     "El juego no pudo empezar:\n{0}\n\nLa copia del juego puede estar dañada o incompleta. La próxima vez que abras el programa se abrirá el lanzador: elige otra vez la carpeta del juego, o copia de nuevo los archivos del juego.",
     "ゲームを開始できませんでした:\n{0}\n\nゲームのコピーが壊れているか不完全かもしれません。次回の起動ではランチャーが開きます。フォルダーを選び直すか、ファイルをコピーし直してください。"},
    {"The game stopped because of an error in its scripts:\n{0}",
     "El juego se detuvo por un error en sus scripts:\n{0}", "ゲームのスクリプトでエラーが起き、停止しました:\n{0}"},
    {"The game could not read its save data and stopped:\n{0}\n\nThe save files were moved to:\n{1}\n\nThe next start is a clean one; copy the files back if you want to try them again.",
     "El juego no pudo leer sus datos de guardado y se detuvo:\n{0}\n\nLos archivos de guardado se movieron a:\n{1}\n\nLa próxima vez empieza limpio; copia los archivos de vuelta si quieres intentarlo otra vez.",
     "ゲームがセーブデータを読めず、停止しました:\n{0}\n\nセーブファイルは次の場所に移動しました:\n{1}\n\n次回は新しい状態で起動します。もう一度試すには、ファイルを元に戻してください。"},
    {"Your settings could not be saved in:\n{0}\n\nThe folder may be read-only. The program works, but your choices will not be remembered.",
     "No se pudieron guardar tus ajustes en:\n{0}\n\nLa carpeta puede ser de solo lectura. El programa funciona, pero no recordará tus elecciones.",
     "設定を次の場所に保存できませんでした:\n{0}\n\nフォルダーが読み取り専用かもしれません。プログラムは動きますが、設定は記憶されません。"},
    {"Could not copy {0}. Check that your folder can be read and that the disk has room, then try again.",
     "No se pudo copiar {0}. Comprueba que tu carpeta se puede leer y que el disco tiene espacio, y vuelve a intentarlo.",
     "{0}をコピーできませんでした。フォルダーを読み取れるか、ディスクに空きがあるか確かめて、もう一度お試しください。"},
    {"Could not finish {0}. Check that the disk has room and that no other program is using the folder, then try again.",
     "No se pudo terminar {0}. Comprueba que el disco tiene espacio y que ningún otro programa usa la carpeta, y vuelve a intentarlo.",
     "{0}を完了できませんでした。ディスクに空きがあるか、他のプログラムがフォルダーを使っていないか確かめて、もう一度お試しください。"},
    {"Could not read {0} from your copy. If the copy is damaged, copy the game's files again.",
     "No se pudo leer {0} de tu copia. Si la copia está dañada, copia de nuevo los archivos del juego.",
     "お持ちのコピーから{0}を読み込めませんでした。コピーが壊れている場合は、ファイルをコピーし直してください。"},
    // --- the launcher's settings -----------------------------------------------------------------------------------
    {"Open saves folder", "Carpeta de partidas", "セーブフォルダーを開く"},
    {"Controls...", "Controles...", "操作..."},
    {"Delete copy of the game...", "Borrar la copia del juego...", "ゲームのコピーを削除..."},
    {"Add English patch...", "Parche en inglés...", "英語パッチを追加..."},
    {"Desktop shortcut", "Acceso directo", "ショートカット"},
    {"Pad mapping...", "Mapeo de mando...", "パッドの割り当て..."},
    {"Launcher language", "Idioma del lanzador", "ランチャーの言語"},
    {"Automatic (Windows)", "Automático (Windows)", "自動(Windows)"},
    {"This copy's own (English)", "El de esta copia (inglés)", "このコピーのもの(英語)"},
    {"Original (Japanese)", "Original (japonés)", "オリジナル(日本語)"},
    {"English (fan patch)", "Inglés (parche de fans)", "英語(ファンパッチ)"},
    {"There is no copy of the game in the program's own folder.", "No hay ninguna copia del juego en la carpeta del programa.",
     "プログラムのフォルダーにゲームのコピーはありません。"},
    {"Delete the copy of the game in:\n{0}\n\nYour saves and settings are kept. You can copy the game again from your own folder.",
     "¿Borrar la copia del juego en:\n{0}\n\nTus partidas y ajustes se conservan. Puedes volver a copiar el juego desde tu propia carpeta.",
     "次の場所にあるゲームのコピーを削除しますか?\n{0}\n\nセーブデータと設定は残ります。ご自身のフォルダーからまたコピーできます。"},
    {"Amagami is now on the desktop and in the Start menu.", "Amagami ya está en el escritorio y en el menú Inicio.",
     "Amagamiをデスクトップとスタートメニューに追加しました。"},
    {"The desktop shortcut could not be made.", "No se pudo crear el acceso directo del escritorio.", "デスクトップのショートカットを作れませんでした。"},
    {"The Start menu shortcut could not be made.", "No se pudo crear el acceso directo del menú Inicio.", "スタートメニューのショートカットを作れませんでした。"},
    {"The program's own path could not be read.", "No se pudo leer la ruta del propio programa.", "プログラム自身の場所を読み取れませんでした。"},
    {"Put a shortcut to Amagami on the desktop and in the Start menu?\n\nYou can also make them later in General, Desktop shortcut.",
     "¿Poner un acceso directo a Amagami en el escritorio y en el menú Inicio?\n\nTambién puedes hacerlos más tarde en General, Acceso directo.",
     "Amagamiのショートカットをデスクトップとスタートメニューに作りますか?\n\n後から「全般」の「ショートカット」でも作れます。"},
    // --- the pad mapping window ------------------------------------------------------------------------------------
    {"Pad mapping", "Mapeo de mando", "パッドの割り当て"},
    {"If your pad is not recognised, paste its mapping line here (one per line): the kind the SDL2 Gamepad Tool makes, or a line of the community's gamecontrollerdb.txt. It starts with a long code, then the pad's name, then entries such as a:b0. The line is kept and used the next time the game starts.",
     "Si tu mando no se reconoce, pega aquí su línea de mapeo (una por línea): la que crea la SDL2 Gamepad Tool o una línea del gamecontrollerdb.txt de la comunidad. Empieza con un código largo, luego el nombre del mando y luego entradas como a:b0. La línea se guarda y se usa la próxima vez que se inicie el juego.",
     "パッドが認識されない場合は、割り当ての行をここに貼り付けてください(1行に1つ)。SDL2 Gamepad Toolで作った行や、コミュニティのgamecontrollerdb.txtの行が使えます。長いコードで始まり、パッド名、a:b0のような項目が続きます。行は保存され、次にゲームを起動したときに使われます。"},
    {"Those mappings were already saved.", "Esos mapeos ya estaban guardados.", "その割り当てはすでに保存されています。"},
    {"Saved. The pad is used the next time the game starts.", "Guardado. El mando se usará la próxima vez que se inicie el juego.",
     "保存しました。次にゲームを起動したときにパッドが使われます。"},
    {"Line {0} is not a mapping: it should start with the pad's 32-digit code, then its name, then entries such as a:b0.",
     "La línea {0} no es un mapeo: debe empezar con el código de 32 dígitos del mando, luego su nombre y luego entradas como a:b0.",
     "{0}行目は割り当てではありません。パッドの32桁のコード、名前、a:b0のような項目の順になっている必要があります。"},
    {"There is no place to keep the mapping.", "No hay dónde guardar el mapeo.", "割り当てを保存する場所がありません。"},
    {"The file could not be written: {0}", "No se pudo escribir el archivo: {0}", "ファイルに書き込めませんでした: {0}"},
    // --- the keyboard controls window ------------------------------------------------------------------------------
    {"Controls", "Controles", "操作"},
    {"Click the key or the pad button of a row, then press the new one. Right click on a pad button: automatic.",
     "Haz clic en la tecla o en el botón del mando de una fila y luego pulsa el nuevo. Clic derecho en un botón del mando: automático.",
     "行のキーまたはパッドのボタンをクリックしてから、新しいものを押してください。パッドのボタンを右クリックすると自動に戻ります。"},
    {"Press a pad button...", "Pulsa un botón del mando...", "パッドのボタンを押してください..."},
    {"Press the button of your pad for this one (Esc: keep the old one; right click: automatic).",
     "Pulsa el botón de tu mando para este (Esc: conservar el anterior; clic derecho: automático).",
     "このボタンに使うパッドのボタンを押してください(Esc: 変更しない、右クリック: 自動)。"},
    {"Automatic", "Automático", "自動"},
    {"South button", "Botón sur (abajo)", "下のボタン"},
    {"East button", "Botón este (derecha)", "右のボタン"},
    {"West button", "Botón oeste (izquierda)", "左のボタン"},
    {"North button", "Botón norte (arriba)", "上のボタン"},
    {"Left shoulder", "Hombro izquierdo", "左ショルダー"},
    {"Right shoulder", "Hombro derecho", "右ショルダー"},
    {"Left trigger", "Gatillo izquierdo", "左トリガー"},
    {"Right trigger", "Gatillo derecho", "右トリガー"},
    {"Back button", "Botón Back", "バックボタン"},
    {"Start button", "Botón Start", "スタートボタン"},
    {"D-pad up", "Cruceta arriba", "十字キー上"},
    {"D-pad down", "Cruceta abajo", "十字キー下"},
    {"D-pad left", "Cruceta izquierda", "十字キー左"},
    {"D-pad right", "Cruceta derecha", "十字キー右"},
    {"Left stick click", "Pulsar stick izquierdo", "左スティック押し込み"},
    {"Right stick click", "Pulsar stick derecho", "右スティック押し込み"},
    {"Restore defaults", "Restaurar valores", "初期設定に戻す"},
    {"Press a key...", "Pulsa una tecla...", "キーを押してください..."},
    {"Press the key for this button (Esc: keep the old one).", "Pulsa la tecla para este botón (Esc: conservar la anterior).",
     "このボタンのキーを押してください(Esc: 変更しない)。"},
    {"The default keys are back.", "Volvieron las teclas por defecto.", "初期のキーに戻しました。"},
    {"{0} belongs to the window (Esc quits, F8 settings, F9 screenshot, F10 dump, F11 full screen).",
     "{0} es de la ventana (Esc sale, F8 ajustes, F9 captura, F10 volcado, F11 pantalla completa).",
     "{0}はウィンドウ用です(Escで終了、F8で設定、F9でスクリーンショット、F10でダンプ、F11で全画面)。"},
    {"{0} is already {1}. Give that one another key first.", "{0} ya es {1}. Primero cambia la tecla de ese.",
     "{0}はすでに「{1}」に使われています。先にそちらのキーを変えてください。"},
    {"Up", "Arriba", "上"},
    {"Down", "Abajo", "下"},
    {"Left", "Izquierda", "左"},
    {"Right", "Derecha", "右"},
    {"Confirm (circle)", "Aceptar (círculo)", "決定(○)"},
    {"Cancel (cross)", "Cancelar (cruz)", "キャンセル(×)"},
    {"Square", "Cuadrado", "□"},
    {"Triangle", "Triángulo", "△"},
    {"L", "L", "L"},
    {"R", "R", "R"},
    {"L2", "L2", "L2"},
    {"R2", "R2", "R2"},
    {"Start", "Start", "スタート"},
    {"Select", "Select", "セレクト"},
    {"Up arrow", "Flecha arriba", "↑キー"},
    {"Down arrow", "Flecha abajo", "↓キー"},
    {"Left arrow", "Flecha izquierda", "←キー"},
    {"Right arrow", "Flecha derecha", "→キー"},
    {"Enter", "Intro", "Enter"},
    {"Backspace", "Retroceso", "Backspace"},
    {"Shift", "Mayús", "Shift"},
    {"Space", "Espacio", "スペース"},
    {"Tab", "Tab", "Tab"},
    // --- the rows of the port's settings ---------------------------------------------------------------------------
    {"Interface size", "Tamaño de la interfaz", "インターフェースの大きさ"},
    {"Render scale", "Escala de dibujo", "描画の解像度"},
    {"Screen filter", "Filtro de pantalla", "画面フィルター"},
    {"Screen shape", "Forma de la pantalla", "画面の形"},
    {"Button pictures", "Imágenes de los botones", "ボタンの絵"},
    {"Confirm button", "Botón de aceptar", "決定ボタン"},
    {"Unknown pad", "Mando desconocido", "不明なパッド"},
    {"Splash screen", "Pantalla de inicio", "スプラッシュ画面"},
    {"Launcher at start", "Lanzador al iniciar", "起動時のランチャー"},
    {"Check for updates", "Buscar actualizaciones", "更新の確認"},
    {"Auto", "Auto", "自動"},
    {"Smooth", "Suave", "なめらか"},
    {"Sharp", "Nítido", "シャープ"},
    {"Pixels", "Píxeles", "ドット"},
    {"Fit", "Ajustar", "収める"},
    {"Fill", "Rellenar", "埋める"},
    {"Stretch", "Estirar", "引き伸ばし"},
    {"Whole", "Entera", "全体"},
    {"Keyboard", "Teclado", "キーボード"},
    {"South", "Abajo", "下"},
    {"East", "Derecha", "右"},
    {"On", "Sí", "オン"},
    {"Off", "No", "オフ"},
    {"Show", "Mostrar", "表示"},
    {"Skip (Shift shows it)", "Omitir (Mayús lo muestra)", "表示しない(Shiftで表示)"},
    // --- importing the game and the English patch ------------------------------------------------------------------
    {"Amagami: importing the game", "Amagami: importando el juego", "Amagami: ゲームを取り込み中"},
    {"Amagami: importing the game, {0} %", "Amagami: importando el juego, {0} %", "Amagami: ゲームを取り込み中、{0} %"},
    {"{0}\n{1} of {2} GB", "{0}\n{1} de {2} GB", "{0}\n{1} / {2} GB"},
    {" (already there)", " (ya está)", "(取り込み済み)"},
    {"There is not enough free space for the game's files (about {0} GB needed) in {1}.",
     "No hay espacio libre suficiente para los archivos del juego (hacen falta unos {0} GB) en {1}.",
     "{1}にゲームのファイルを入れる空き容量が足りません(約{0} GBが必要です)。"},
    {"Could not write {0} (is the disk full?).", "No se pudo escribir {0} (¿el disco está lleno?).", "{0}を書き込めませんでした(ディスクがいっぱいですか?)。"},
    {"Cancelled.", "Cancelado.", "キャンセルしました。"},
    {"Choose your game folder first.", "Primero elige la carpeta de tu juego.", "先にゲームのフォルダーを選んでください。"},
    {"Your game folder already has the English translation built in: nothing to add.",
     "La carpeta de tu juego ya trae la traducción al inglés: no hay nada que añadir.", "ゲームフォルダーには英語翻訳がすでに組み込まれています。追加するものはありません。"},
    {"Choose the folder of the English fan patch (Amagami EbKore+ English v1.1)", "Elige la carpeta del parche de fans en inglés (Amagami EbKore+ English v1.1)",
     "英語ファンパッチ(Amagami EbKore+ English v1.1)のフォルダーを選んでください"},
    {"No AMAGAMI.IMG, SCENARIO.CPK and GRAPH.CPK were found together in that folder. Choose the patch's folder that holds them (for the NoNpDrm release, its \"repatch\" folder).",
     "En esa carpeta no se encontraron juntos AMAGAMI.IMG, SCENARIO.CPK y GRAPH.CPK. Elige la carpeta del parche que los tiene (en la versión NoNpDrm, su carpeta \"repatch\").",
     "そのフォルダーにはAMAGAMI.IMG、SCENARIO.CPK、GRAPH.CPKが揃っていませんでした。これらがあるパッチのフォルダーを選んでください(NoNpDrm版では「repatch」フォルダーです)。"},
    {"Those are the original Japanese files, not the translation. Add them anyway?", "Esos son los archivos originales en japonés, no la traducción. ¿Añadirlos igualmente?",
     "これは翻訳ではなく、元の日本語のファイルです。それでも追加しますか?"},
    {"Those files are not the English patch this program knows (v1.1). They may be another version. Add them anyway?",
     "Esos archivos no son el parche en inglés que conoce este programa (v1.1). Puede ser otra versión. ¿Añadirlos igualmente?",
     "これはこのプログラムが知っている英語パッチ(v1.1)ではありません。別のバージョンかもしれません。それでも追加しますか?"},
    {"Could not copy {0}:\n{1}", "No se pudo copiar {0}:\n{1}", "{0}をコピーできませんでした:\n{1}"},
    {"The English translation was added and chosen. You can switch back in Game files, Language.",
     "Se añadió y eligió la traducción al inglés. Puedes volver atrás en Archivos del juego, Idioma.", "英語翻訳を追加して選択しました。「ゲームファイル」の「言語」で元に戻せます。"},
    {"This program needs your own copy of the game: the PS Vita version of Amagami (PCSG00291), already decrypted. Choose its folder next.",
     "Este programa necesita tu propia copia del juego: la versión de PS Vita de Amagami (PCSG00291), ya descifrada. A continuación elige su carpeta.",
     "このプログラムにはお持ちのゲームのコピーが必要です: Amagami PS Vita版(PCSG00291)の復号済みのもの。次にそのフォルダーを選んでください。"},
    {"{0}\n\nChoose another folder?", "{0}\n\n¿Elegir otra carpeta?", "{0}\n\n別のフォルダーを選びますか?"},
    {"Choose the folder of your PS Vita copy of Amagami (the one with AMAGAMI.IMG and SCENARIO.CPK)",
     "Elige la carpeta de tu copia de Amagami para PS Vita (la que tiene AMAGAMI.IMG y SCENARIO.CPK)",
     "お持ちのAmagami PS Vita版のコピーのフォルダー(AMAGAMI.IMGとSCENARIO.CPKがあるもの)を選んでください"},
    {"Copy the game into this program's own folder (about {0} GB)? Then you can delete or move your original copy afterwards.\n\nYes: copy it.   No: run it from where it is.",
     "¿Copiar el juego a la carpeta del propio programa (unos {0} GB)? Después puedes borrar o mover tu copia original.\n\nSí: copiarlo.   No: ejecutarlo desde donde está.",
     "ゲームをこのプログラムのフォルダーにコピーしますか(約{0} GB)? その後、元のコピーは削除や移動ができます。\n\nはい: コピーする。   いいえ: 今の場所から起動する。"},
    {"Quit the game?\nProgress that has not been saved is lost.", "¿Salir del juego?\nSe pierde el progreso que no se haya guardado.",
     "ゲームを終了しますか?\nセーブしていない進行状況は失われます。"},
};

Lang g_lang = Lang::En;
std::unordered_map<std::string, const Row*>& index() {
    static std::unordered_map<std::string, const Row*> table = [] {
        std::unordered_map<std::string, const Row*> t;
        for (const Row& r : ROWS) t[r.en] = &r;
        return t;
    }();
    return table;
}

}  // namespace

void init(const std::string& setting) {
    if (setting == "es") {
        g_lang = Lang::Es;
    } else if (setting == "ja") {
        g_lang = Lang::Ja;
    } else if (setting == "en") {
        g_lang = Lang::En;
    } else {
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
            case LANG_SPANISH: g_lang = Lang::Es; break;
            case LANG_JAPANESE: g_lang = Lang::Ja; break;
            default: g_lang = Lang::En; break;
        }
    }
}

Lang language() { return g_lang; }

std::string tr(const std::string& english) {
    if (g_lang == Lang::En) return english;
    auto it = index().find(english);
    if (it == index().end()) return english;
    return g_lang == Lang::Es ? it->second->es : it->second->ja;
}

std::string fmt(const std::string& english, std::initializer_list<std::string> values) {
    std::string text = tr(english);
    int n = 0;
    for (const std::string& value : values) {
        const std::string mark = "{" + std::to_string(n++) + "}";
        for (size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at + value.size())) text.replace(at, mark.size(), value);
    }
    return text;
}

// UTF-8 as it is; a byte that is not part of a valid sequence is taken as in the system code page (a path the program got from
// the system in that code page, put into a message).
std::wstring wide(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool valid = length != 0 && i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k) valid = ((unsigned char)text[i + k] & 0xC0) == 0x80;
        wchar_t buffer[4];
        int n = 0;
        if (valid) n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data() + i, int(length), buffer, 4);
        if (valid && n > 0) {
            out.append(buffer, size_t(n));
            i += length;
        } else {
            length = IsDBCSLeadByte(c) && i + 1 < text.size() ? 2 : 1;
            n = MultiByteToWideChar(CP_ACP, 0, text.data() + i, int(length), buffer, 4);
            if (n > 0) out.append(buffer, size_t(n));
            else out.push_back(L'?');
            i += length;
        }
    }
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), char(0));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wtr(const std::string& english) { return wide(tr(english)); }

std::wstring wfmt(const std::string& english, std::initializer_list<std::string> values) { return wide(fmt(english, values)); }

}  // namespace amg::ui
