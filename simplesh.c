// Shell `simplesh`
#define _XOPEN_SOURCE 500 
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <libgen.h>
#include <getopt.h>
#include <ftw.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

// Libreadline
#include <readline/readline.h>
#include <readline/history.h>


// Tipos presentes en la estructura `cmd`, campo `type`.
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 15

#define MAXPATH 256

#define READSIZE 512

#define AOPENFLAGS O_WRONLY|O_APPEND|O_CREAT
#define OPENFLAGS O_WRONLY|O_APPEND|O_CREAT

// Estructuras
// -----

// La estructura `cmd` se utiliza para almacenar la información que
// servirá al shell para guardar la información necesaria para
// ejecutar las diferentes tipos de órdenes (tuberías, redirecciones,
// etc.)
//
// El formato es el siguiente:
//
//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|
//
// Nótese cómo las estructuras `cmd` comparten el primer campo `type`
// para identificar el tipo de la estructura, y luego se obtendrá un
// tipo derivado a través de *casting* forzado de tipo. Se obtiene así
// un polimorfismo básico en C.
struct cmd {
    int type;
};

// Ejecución de un comando con sus parámetros
struct execcmd {
    int type;
    char * argv[MAXARGS];
    char * eargv[MAXARGS];
};

// Ejecución de un comando de redirección
struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

// Ejecución de un comando de tubería
struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

// Lista de órdenes
struct listcmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

// Tarea en segundo plano (background) con `&`.
struct backcmd {
    int type;
    struct cmd *cmd;
};

// Declaración de funciones necesarias
int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parse_cmd(char*);

// Función para implementar el comando pwd como un comando interno.
void run_pwd(){
    char path[MAXPATH];
    char * ruta = getcwd(path, MAXPATH);
    if (ruta == NULL){
        perror("getcwd");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "simplesh: pwd: ");
    fprintf(stdout, "%s\n", ruta);
}

// Función para implementar el comando cd como un comando interno.
void run_cd(struct cmd *command){
    struct execcmd *comm = (struct execcmd*)command;
    char *route;
    if (comm->argv[1] == NULL)
        route = getenv("HOME");
    else
        route = comm->argv[1];
    if (chdir(route) == -1){
        perror("cd");
        if (errno != ENOENT && errno != EACCES && errno != ENOTDIR)
            exit(EXIT_FAILURE);
    }
    
}

void print_teelog(int bytes, int files){
    printf("log");
    int pid, euid;
    struct timeval tv;
    time_t tm;
    char tmbuf[20];
    pid = getpid();
    euid = geteuid();
    
    if(gettimeofday(&tv, NULL) == -1)
        perror("gettimeofday");
    else{
        tm = tv.tv_sec;
        struct tm * tiempo = localtime(&tm);
        strftime(tmbuf, sizeof tmbuf, "%Y-%m-%d %H:%M:%S", tiempo);
        char output[64];
        int chars = sprintf(output, "%s:PID %d:EUID %d:%d byte(s):%d file(s)\n", tmbuf, pid, euid, bytes, files);
        char* ruta = "/.log.tee";
        char * home = getenv("HOME");
        strcat(home, ruta);
        printf("Creando log en %s\n", home);
        int descr = open(home, AOPENFLAGS, S_IRWXU);        
        if (descr != -1){
           if(write(descr, output, chars) == -1)
            perror("write");
        }
        else
            perror("open");
    }
}

// Función para implementar el comando tee como un comando interno
void run_tee(struct execcmd* ecmd){
    int cont=0;
    int opt;
    int aflag = 0;
    int hflag = 0;
    while (ecmd->argv[cont])
        cont++;
    while ((opt = getopt(cont, ecmd->argv, "ha")) != -1){
        switch (opt){
            case 'h':
                hflag = 1;
                break;
            case 'a':
                aflag = 1;
                break;
            case '?':
                hflag = 1;
                break;
        }
    }
    
    if (hflag){
        fprintf(stdout, "Uso: tee [-h] [-a] [FICHERO]\n"\
                            "\tCopia stdin a cada FICHERO y a stdout\n"\
                            "\tOpciones:\n"\
                            "\t-a Añade al final de cada FICHERO\n"\
                            "\t-h help\n");
    }
    else{
        int numFich = cont-optind;
        int descriptor[numFich];
        int flag;
        if (aflag)
            flag = AOPENFLAGS;
        else
            flag = OPENFLAGS;
            
        for (int i = 0; i < numFich; i++) {
            descriptor[i] = open(ecmd->argv[i+optind], flag, S_IRWXU);
            if(descriptor[i] == -1){
                perror("open");
            }
        }
        
        char buf[READSIZE];
        int bytesLeidos = 0;
        int bytesEscritos = 0;
        int aux = 0;
        int bytes = 0;
        while ((bytesLeidos = read(STDIN_FILENO, buf, READSIZE)) > 0){
            bytes += bytesLeidos;
            while (bytesEscritos != bytesLeidos){
                aux = write(STDOUT_FILENO, buf, bytesLeidos);
                if (aux != -1)
                    bytesEscritos+= aux;
                else{
                    perror ("write");
                }
            }
            bytesEscritos = 0;
            for (int i = 0; i < numFich; i++) {
                if (descriptor[i] != -1){
                    while(bytesEscritos != bytesLeidos){
                        aux = write(descriptor[i], buf, bytesLeidos);
                        if (aux != -1)
                            bytesEscritos += aux;
                        else
                            perror("write");
                    }
                    bytesEscritos = 0;
                }
            }
        }
        if (bytesLeidos == -1)
            perror("read");
        
        for (int i = 0; i < numFich; i++){
            if (descriptor[i] != -1){
                if (fsync(descriptor[i]) == -1)
                    perror("fsync");
                if (close(descriptor[i]) == -1)
                    perror("close");
            }
        }
        print_teelog(bytes, numFich);
    }
}

int totalSize;

int du_aux(const char *fpath, const struct stat *sb,
            int tflag, struct FTW *ftwbuf){
    if (S_ISREG(sb->st_mode))
        totalSize += (int) sb->st_size;
    return 0; /* To tell nftw() to continue */
}


void run_du(struct execcmd *ecmd){
    int opt;
    int cont = 0;
    int size;
    int hflag = 0;
    int bflag = 0;
    int vflag = 0;
    int tflag = 0;
    while (ecmd->argv[cont])
        cont++;
     while ((opt = getopt(cont, ecmd->argv, "hbvt:")) != -1){
        switch (opt){
            case 'h':
                hflag = 1;
                break;
            case 'b':
                bflag = 1;
                break;
            case 'v':
                vflag = 1;
                break;
            case 't':
                tflag = 1;
                sscanf(optarg, "%d", &size);
                break;
            case '?':
                hflag = 1;
                break;
        }
    }
    if (hflag){
        
        fprintf(stdout, "Uso : du [-h] [- b] [ -t SIZE ] [-v ] [ FICHERO | DIRECTORIO ]\n"\
        "Para cada fichero, imprime su tamaño.\n"\
        "Para cada directorio, imprime la suma de los tamaños de todos los ficheros de\n"\
            "\ttodos sus subdirectorios.\n"\
            "\tOpciones :\n"\
            "\t-b Imprime el tamaño ocupado en disco por todos los bloques del fichero.\n"\
            "\t-t SIZE Excluye todos los ficheros más pequeños que SIZE bytes, si es\n"\
            "\tpositivo, o más grandes que SIZE bytes, si es negativo, cuando se\n"\
                "\t\tprocesa un directorio .\n"\
            "\t-v Imprime el tamaño de todos y cada uno de los ficheros cuando se procesa un\n"\
                "\t\tdirectorio.\n" \
            "\t-h help\n"\
        "Nota: todos los tamaños están expresados en bytes\n");
    }
    else {
        struct stat st;
        int i = optind;
        do{
            char * path;
            path = i < cont ? ecmd->argv[i] : ".";
            if (stat(path, &st) == -1) {
                perror("stat");
                exit(EXIT_FAILURE);
            }
            if(S_ISDIR(st.st_mode)){
                int flags = 0;
                int nopenfd = 20;
                totalSize = 0;
                if (nftw(path, du_aux, 20, flags) == -1){
                    perror("nftw");
                    exit(EXIT_FAILURE);
                }
                fprintf(stdout,"(D) %s: %d\n", path, totalSize);
            }
            else if (S_ISREG(st.st_mode)) {
                int sizethreshold = 0;
                if (tflag)
                    sizethreshold = size;    
                if ((sizethreshold > 0 && st.st_size < sizethreshold) 
                        || (sizethreshold < 0 && st.st_size > sizethreshold)
                        || sizethreshold == 0){
                    fprintf(stdout,"(F) ");
                    if (bflag)
                        fprintf(stdout, "%s: %zu\n", path, st.st_blocks*512);
                    else 
                        fprintf(stdout, "%s: %zu\n", path, st.st_size);
                }
            }
            i++;
        } while (i < cont);
    }
}

// Ejecuta un `cmd`. Nunca retorna, ya que siempre se ejecuta en un
// hijo lanzado con `fork()`.
void run_cmd(struct cmd *cmd){
    int p[2];
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0)
        exit(0);

    switch(cmd->type)
    {
    default:
        panic("run_cmd");

        // Ejecución de una única orden.
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        if (ecmd->argv[0] == 0)
            exit(0);
        else if (strcmp(ecmd->argv[0], "pwd") == 0)
            run_pwd();
        else if (strcmp(ecmd->argv[0], "tee") == 0)
            run_tee(ecmd);
        else if (strcmp(ecmd->argv[0], "du") == 0)
            run_du(ecmd);
        else{
            execvp(ecmd->argv[0], ecmd->argv);
            // Si se llega aquí algo falló
            fprintf(stderr, "exec %s failed\n", ecmd->argv[0]);
            exit (1);
        }
        break;

    case REDIR:
        rcmd = (struct redircmd*)cmd;
        close(rcmd->fd);
        if (open(rcmd->file, rcmd->mode, S_IRWXU) < 0)
        {
            fprintf(stderr, "open %s failed\n", rcmd->file);
            exit(1);
        }
        run_cmd(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        if (fork1() == 0)
            run_cmd(lcmd->left);
        wait(NULL);
        run_cmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        if (pipe(p) < 0)
            panic("pipe");

        // Ejecución del hijo de la izquierda
        if (fork1() == 0)
        {
            close(1);
            dup(p[1]);
            close(p[0]);
            close(p[1]);
            run_cmd(pcmd->left);
        }

        // Ejecución del hijo de la derecha
        if (fork1() == 0)
        {
            close(0);
            dup(p[0]);
            close(p[0]);
            close(p[1]);
            run_cmd(pcmd->right);
        }
        close(p[0]);
        close(p[1]);

        // Esperar a ambos hijos
        wait(NULL);
        wait(NULL);
        break;

    case BACK:
        bcmd = (struct backcmd*)cmd;
        if (fork1() == 0)
            run_cmd(bcmd->cmd);
        break;
    }

    // Salida normal, código 0.
    exit(0);
}

// Muestra un *prompt* y lee lo que el usuario escribe usando la
// librería readline. Ésta permite almacenar en el historial, utilizar
// las flechas para acceder a las órdenes previas, búsquedas de
// órdenes, etc.
char* getcmd() {
    char *buf;
    int retval = 0;
	char* username = getpwuid(getuid())->pw_name;
	
	if (username == NULL){
		perror("getpwuid");
		exit(EXIT_FAILURE);
	}

	char path[MAXPATH];
	if (getcwd(path, MAXPATH) == NULL){
		perror("getcwd");
		exit(EXIT_FAILURE);
	}
	char prompt[MAXPATH];
	snprintf(prompt, MAXPATH,"%s@%s$ ", username, basename(path));

    // Lee la entrada del usuario
    buf = readline (prompt);

    // Si el usuario ha escrito algo, almacenarlo en la historia.
    if(buf)
        add_history (buf);

    return buf;
}

void run(struct cmd *command){
    
    if (command->type == LIST){
        struct listcmd *lcmd = (struct listcmd *)command;
        run(lcmd->left);
        run(lcmd->right);
    }
    else if ((((struct execcmd*)command)->argv[0] != NULL) && strcmp(((struct execcmd*)command)->argv[0], "exit") == 0)
        exit(0);
    else if ((((struct execcmd*)command)->argv[0] != NULL) && strcmp(((struct execcmd*)command)->argv[0], "cd") == 0)
        run_cd(command);
    // Crear siempre un hijo para ejecutar el comando leído
    else if(fork1() == 0)
        run_cmd(command);
    
    // Esperar al hijo creado
    wait(NULL);
}


// Función `main()`.
// ----

int main(void) {
    char* buf;

    // Bucle de lectura y ejecución de órdenes.
    while (NULL != (buf = getcmd()))
    {
        if (strcmp(buf, "") != 0){
            struct cmd *command = parse_cmd(buf);
            run(command);
        
        }
        free ((void*)buf);
    } 

    return 0;
}

void
panic(char *s)
{
    fprintf(stderr, "%s\n", s);
    exit(-1);
}

// Como `fork()` salvo que muestra un mensaje de error si no se puede
// crear el hijo.
int
fork1(void)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("fork");
    return pid;
}

// Constructores de las estructuras `cmd`.
// ----

// Construye una estructura `EXEC`.
struct cmd*
execcmd(void)
{
    struct execcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd*)cmd;
}

// Construye una estructura de redirección.
struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
    struct redircmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd*)cmd;
}

// Construye una estructura de tubería (*pipe*).
struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
    struct pipecmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

// Construye una estructura de lista de órdenes.
struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
    struct listcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd*)cmd;
}

// Construye una estructura de ejecución que incluye una ejecución en
// segundo plano.
struct cmd*
backcmd(struct cmd *subcmd)
{
    struct backcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd*)cmd;
}

// Parsing
// ----

const char whitespace[] = " \t\r\n\v";
const char symbols[] = "<|>&;()";

// Obtiene un *token* de la cadena de entrada `ps`, y hace que `q` apunte a
// él (si no es `NULL`).
int
gettoken(char **ps, char *end_of_str, char **q, char **eq)
{
    char *s;
    int ret;

    s = *ps;
    while (s < end_of_str && strchr(whitespace, *s))
        s++;
    if (q)
        *q = s;
    ret = *s;
    switch (*s)
    {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>')
        {
            ret = '+';
            s++;
        }
        break;

    default:
        // El caso por defecto (no hay caracteres especiales) es el de un
        // argumento de programa. Se retorna el valor `'a'`, `q` apunta al
        // mismo (si no era `NULL`), y `ps` se avanza hasta que salta todos
        // los espacios **después** del argumento. `eq` se hace apuntar a
        // donde termina el argumento. Así, si `ret` es `'a'`:
        //
        //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
        //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio) |
        //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
        //                   ^                                   ^
        //                   |q                                  |eq
        //
        ret = 'a';
        while (s < end_of_str && !strchr(whitespace, *s) && !strchr(symbols, *s))
            s++;
        break;
    }

    // Apuntar `eq` (si no es `NULL`) al final del argumento.
    if (eq)
        *eq = s;

    // Y finalmente saltar los espacios en blanco y actualizar `ps`.
    while(s < end_of_str && strchr(whitespace, *s))
        s++;
    *ps = s;

    return ret;
}

// La función `peek()` recibe un puntero a una cadena, `ps`, y un final de
// cadena, `end_of_str`, y un conjunto de tokens (`toks`). El puntero
// pasado, `ps`, es llevado hasta el primer carácter que no es un espacio y
// posicionado ahí. La función retorna distinto de `NULL` si encuentra el
// conjunto de caracteres pasado en `toks` justo después de los posibles
// espacios.
int
peek(char **ps, char *end_of_str, char *toks)
{
    char *s;

    s = *ps;
    while(s < end_of_str && strchr(whitespace, *s))
        s++;
    *ps = s;

    return *s && strchr(toks, *s);
}

// Definiciones adelantadas de funciones.
struct cmd *parse_line(char**, char*);
struct cmd *parse_pipe(char**, char*);
struct cmd *parse_exec(char**, char*);
struct cmd *nulterminate(struct cmd*);

// Función principal que hace el *parsing* de una línea de órdenes dada por
// el usuario. Llama a la función `parse_line()` para obtener la estructura
// `cmd`.
struct cmd*
parse_cmd(char *s)
{
    char *end_of_str;
    struct cmd *cmd;

    end_of_str = s + strlen(s);
    cmd = parse_line(&s, end_of_str);

    peek(&s, end_of_str, "");
    if (s != end_of_str)
    {
        fprintf(stderr, "restante: %s\n", s);
        panic("syntax");
    }

    // Termina en `'\0'` todas las cadenas de caracteres de `cmd`.
    nulterminate(cmd);

    return cmd;
}

// *Parsing* de una línea. Se comprueba primero si la línea contiene alguna
// tubería. Si no, puede ser un comando en ejecución con posibles
// redirecciones o un bloque. A continuación puede especificarse que se
// ejecuta en segundo plano (con `&`) o simplemente una lista de órdenes
// (con `;`).
struct cmd*
parse_line(char **ps, char *end_of_str)
{
    struct cmd *cmd;

    cmd = parse_pipe(ps, end_of_str);
    while (peek(ps, end_of_str, "&"))
    {
        gettoken(ps, end_of_str, 0, 0);
        cmd = backcmd(cmd);
    }

    if (peek(ps, end_of_str, ";"))
    {
        gettoken(ps, end_of_str, 0, 0);
        cmd = listcmd(cmd, parse_line(ps, end_of_str));
    }

    return cmd;
}

// *Parsing* de una posible tubería con un número de órdenes.
// `parse_exec()` comprobará la orden, y si al volver el siguiente *token*
// es un `'|'`, significa que se puede ir construyendo una tubería.
struct cmd*
parse_pipe(char **ps, char *end_of_str)
{
    struct cmd *cmd;

    cmd = parse_exec(ps, end_of_str);
    if (peek(ps, end_of_str, "|"))
    {
        gettoken(ps, end_of_str, 0, 0);
        cmd = pipecmd(cmd, parse_pipe(ps, end_of_str));
    }

    return cmd;
}


// Construye los comandos de redirección si encuentra alguno de los
// caracteres de redirección.
struct cmd*
parse_redirs(struct cmd *cmd, char **ps, char *end_of_str)
{
    int tok;
    char *q, *eq;

    // Si lo siguiente que hay a continuación es una redirección...
    while (peek(ps, end_of_str, "<>"))
    {
        // La elimina de la entrada
        tok = gettoken(ps, end_of_str, 0, 0);

        // Si es un argumento, será el nombre del fichero de la
        // redirección. `q` y `eq` tienen su posición.
        if (gettoken(ps, end_of_str, &q, &eq) != 'a')
            panic("missing file for redirection");

        switch(tok)
        {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_RDWR|O_CREAT|O_TRUNC, 1);
            break;
        case '+':  // >>
            cmd = redircmd(cmd, q, eq, O_RDWR|O_CREAT|O_APPEND, 1);
            break;
        }
    }

    return cmd;
}

// *Parsing* de un bloque de órdenes delimitadas por paréntesis.
struct cmd*
parse_block(char **ps, char *end_of_str)
{
    struct cmd *cmd;

    // Esperar e ignorar el paréntesis
    if (!peek(ps, end_of_str, "("))
        panic("parse_block");
    gettoken(ps, end_of_str, 0, 0);

    // Parse de toda la línea hsta el paréntesis de cierre
    cmd = parse_line(ps, end_of_str);

    // Elimina el paréntesis de cierre
    if (!peek(ps, end_of_str, ")"))
        panic("syntax - missing )");
    gettoken(ps, end_of_str, 0, 0);

    // ¿Posibles redirecciones?
    cmd = parse_redirs(cmd, ps, end_of_str);

    return cmd;
}

// Hace en *parsing* de una orden, a no ser que la expresión comience por
// un paréntesis. En ese caso, se inicia un grupo de órdenes para ejecutar
// las órdenes de dentro del paréntesis (llamando a `parse_block()`).
struct cmd*
parse_exec(char **ps, char *end_of_str)
{
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    // ¿Inicio de un bloque?
    if (peek(ps, end_of_str, "("))
        return parse_block(ps, end_of_str);

    // Si no, lo primero que hay una línea siempre es una orden. Se
    // construye el `cmd` usando la estructura `execcmd`.
    ret = execcmd();
    cmd = (struct execcmd*)ret;

    // Bucle para separar los argumentos de las posibles redirecciones.
    argc = 0;
    ret = parse_redirs(ret, ps, end_of_str);
    while (!peek(ps, end_of_str, "|)&;"))
    {
        if ((tok=gettoken(ps, end_of_str, &q, &eq)) == 0)
            break;

        // Aquí tiene que reconocerse un argumento, ya que el bucle para
        // cuando hay un separador
        if (tok != 'a')
            panic("syntax");

        // Apuntar el siguiente argumento reconocido. El primero será la
        // orden a ejecutar.
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if (argc >= MAXARGS)
            panic("too many args");

        // Y de nuevo apuntar posibles redirecciones
        ret = parse_redirs(ret, ps, end_of_str);
    }

    // Finalizar las líneas de órdenes
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}

// Termina en NUL todas las cadenas de `cmd`.
struct cmd*
nulterminate(struct cmd *cmd)
{
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd*)cmd;
        for(i=0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDIR:
        rcmd = (struct redircmd*)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd*)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd*)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd*)cmd;
        nulterminate(bcmd->cmd);
        break;
    }

    return cmd;
}

/*
 * Local variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 75
 * eval: (auto-fill-mode t)
 * End:
 */