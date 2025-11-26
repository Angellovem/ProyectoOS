#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define MIN_HOUR 7
#define MAX_HOUR 19
#define MAX_NAME_LEN 64
#define MAX_FAMILY_LEN 64
#define MAX_LINE_LEN 512

typedef struct {
    char nombre[MAX_NAME_LEN];
    char fileSolicitud[256];
    char pipeRecibe[256];
    char fifoRespuesta[256];
} ConfigAgente;

static void uso(const char *prog) {
    fprintf(stderr,
            "Uso: %s -s nombre -a fileSolicitud -p pipeRecibe\n",
            prog);
}

static int parse_args(int argc, char *argv[], ConfigAgente *cfg) {
    int opt;
    int got_s = 0, got_a = 0, got_p = 0;

    memset(cfg, 0, sizeof(*cfg));

    while ((opt = getopt(argc, argv, "s:a:p:")) != -1) {
        switch (opt) {
            case 's':
                strncpy(cfg->nombre, optarg, sizeof(cfg->nombre) - 1);
                cfg->nombre[sizeof(cfg->nombre) - 1] = '\0';
                got_s = 1;
                break;
            case 'a':
                strncpy(cfg->fileSolicitud, optarg, sizeof(cfg->fileSolicitud) - 1);
                cfg->fileSolicitud[sizeof(cfg->fileSolicitud) - 1] = '\0';
                got_a = 1;
                break;
            case 'p':
                strncpy(cfg->pipeRecibe, optarg, sizeof(cfg->pipeRecibe) - 1);
                cfg->pipeRecibe[sizeof(cfg->pipeRecibe) - 1] = '\0';
                got_p = 1;
                break;
            default:
                uso(argv[0]);
                return -1;
        }
    }

    if (!got_s || !got_a || !got_p) {
        uso(argv[0]);
        return -1;
    }

    if (cfg->nombre[0] == '\0') {
        fprintf(stderr, "Nombre de agente invalido.\n");
        return -1;
    }

    return 0;
}

static void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

// Crea un FIFO unico para este agente basado en nombre y PID.
static int crear_fifo_respuesta(ConfigAgente *cfg) {
    pid_t pid = getpid();
    snprintf(cfg->fifoRespuesta, sizeof(cfg->fifoRespuesta),
             "/tmp/agente_%s_%d.fifo", cfg->nombre, (int)pid);

    if (mkfifo(cfg->fifoRespuesta, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo (fifoRespuesta)");
            return -1;
        }
    }
    return 0;
}

// Envia por el pipeRecibe un mensaje terminado en '\n'.
static int enviar_linea_controlador(int fdCtrl, const char *linea) {
    size_t len = strlen(linea);
    ssize_t written = write(fdCtrl, linea, len);
    if (written != (ssize_t)len) {
        perror("write pipeRecibe");
        return -1;
    }
    if (write(fdCtrl, "\n", 1) != 1) {
        perror("write newline pipeRecibe");
        return -1;
    }
    return 0;
}

// Lee una linea del FIFO de respuesta (bloqueante).
static int leer_linea_fifo(FILE *fp, char *buf, size_t sz) {
    if (!fgets(buf, (int)sz, fp)) {
        if (feof(fp)) {
            // EOF inesperado
            return 0;
        }
        perror("fgets fifoRespuesta");
        return 0;
    }
    trim_newline(buf);
    return 1;
}

// Procesa y muestra un mensaje RESP|... de forma amigable.
static void imprimir_respuesta(const char *linea) {
    char copia[MAX_LINE_LEN];
    strncpy(copia, linea, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';

    char *rest = NULL;
    char *tipo = strtok_r(copia, "|", &rest);  // RESP
    if (!tipo) return;

    if (strcmp(tipo, "RESP") != 0) {
        fprintf(stderr, "Mensaje desconocido del controlador: %s\n", linea);
        return;
    }

    char *subtipo = strtok_r(NULL, "|", &rest);
    char *familia = strtok_r(NULL, "|", &rest);
    char *horaIniStr = strtok_r(NULL, "|", &rest);
    char *horaFinStr = strtok_r(NULL, "|", &rest);

    if (!subtipo || !familia || !horaIniStr || !horaFinStr) {
        fprintf(stderr, "Mensaje RESP mal formado: %s\n", linea);
        return;
    }

    int horaIni = atoi(horaIniStr);
    int horaFin = atoi(horaFinStr);

    if (strcmp(subtipo, "OK") == 0) {
        printf("Familia %s: reserva ACEPTADA de %d a %d horas.\n",
               familia, horaIni, horaFin);
    } else if (strcmp(subtipo, "REPROG") == 0) {
        printf("Familia %s: reserva REPROGRAMADA de %d a %d horas.\n",
               familia, horaIni, horaFin);
    } else if (strcmp(subtipo, "NEG") == 0) {
        printf("Familia %s: reserva NEGADA (sin cupo o parametros invalidos).\n",
               familia);
    } else if (strcmp(subtipo, "NEG_EXTEMP") == 0) {
        printf("Familia %s: reserva NEGADA por extemporanea, sin bloques alternativos.\n",
               familia);
    } else {
        printf("Respuesta desconocida del controlador: %s\n", linea);
    }
}

int main(int argc, char *argv[]) {
    ConfigAgente cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        return EXIT_FAILURE;
    }

    if (crear_fifo_respuesta(&cfg) != 0) {
        return EXIT_FAILURE;
    }

    // Abrir FIFO de respuesta en modo lectura/escritura.
    // Usar O_RDWR evita bloqueos y hace que el FIFO siempre tenga
    // al menos un lector y un escritor, facilitando que el controlador
    // pueda abrirlo en modo solo escritura.
    int fdResp = open(cfg.fifoRespuesta, O_RDWR);
    if (fdResp == -1) {
        perror("open fifoRespuesta");
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    FILE *fpResp = fdopen(fdResp, "r");
    if (!fpResp) {
        perror("fdopen fifoRespuesta");
        close(fdResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    // Abrir pipeRecibe para escritura (hacia el controlador)
    int fdCtrl = open(cfg.pipeRecibe, O_WRONLY);
    if (fdCtrl == -1) {
        perror("open pipeRecibe");
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    // Enviar mensaje de registro
    char linea[MAX_LINE_LEN];
    snprintf(linea, sizeof(linea), "REG|%s|%s", cfg.nombre, cfg.fifoRespuesta);
    if (enviar_linea_controlador(fdCtrl, linea) != 0) {
        close(fdCtrl);
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    // Esperar TIME|horaActual
    int horaActual = MIN_HOUR;
    if (!leer_linea_fifo(fpResp, linea, sizeof(linea))) {
        fprintf(stderr, "No se pudo leer TIME desde el controlador.\n");
        close(fdCtrl);
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    char copia[MAX_LINE_LEN];
    strncpy(copia, linea, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';
    char *rest = NULL;
    char *tipo = strtok_r(copia, "|", &rest);
    if (!tipo || strcmp(tipo, "TIME") != 0) {
        fprintf(stderr, "Mensaje inesperado del controlador (se esperaba TIME): %s\n", linea);
        close(fdCtrl);
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }
    char *horaStr = strtok_r(NULL, "|", &rest);
    if (!horaStr) {
        fprintf(stderr, "Mensaje TIME mal formado: %s\n", linea);
        close(fdCtrl);
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }
    horaActual = atoi(horaStr);
    printf("Agente %s registrado. Hora actual de simulacion: %d\n",
           cfg.nombre, horaActual);

    // Abrir archivo de solicitudes
    FILE *fpCSV = fopen(cfg.fileSolicitud, "r");
    if (!fpCSV) {
        perror("fopen fileSolicitud");
        close(fdCtrl);
        fclose(fpResp);
        unlink(cfg.fifoRespuesta);
        return EXIT_FAILURE;
    }

    // Bucle de lectura del archivo CSV y envio de solicitudes
    char lineaCSV[MAX_LINE_LEN];
    while (fgets(lineaCSV, sizeof(lineaCSV), fpCSV)) {
        trim_newline(lineaCSV);
        if (lineaCSV[0] == '\0') continue;       // linea vacia
        if (lineaCSV[0] == '#') continue;        // comentario

        // Formato: Familia,hora,personas
        char buf[MAX_LINE_LEN];
        strncpy(buf, lineaCSV, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *restCSV = NULL;
        char *familia = strtok_r(buf, ",", &restCSV);
        char *horaStrCSV = strtok_r(NULL, ",", &restCSV);
        char *persStrCSV = strtok_r(NULL, ",", &restCSV);

        if (!familia || !horaStrCSV || !persStrCSV) {
            fprintf(stderr, "Linea CSV mal formada, se ignora: %s\n", lineaCSV);
            continue;
        }

        int hora = atoi(horaStrCSV);
        int personas = atoi(persStrCSV);

        if (hora < MIN_HOUR || hora > MAX_HOUR || personas <= 0) {
            fprintf(stderr, "Solicitud invalida en archivo (rango/aforo), se ignora: %s\n",
                    lineaCSV);
            continue;
        }

        if (hora < horaActual) {
            printf("Solicitud ignorada por ser anterior a la hora actual (%d): %s\n",
                   horaActual, lineaCSV);
            continue;
        }

        // Enviar solicitud REQ
        snprintf(linea, sizeof(linea), "REQ|%s|%s|%d|%d",
                 cfg.nombre, familia, hora, personas);
        if (enviar_linea_controlador(fdCtrl, linea) != 0) {
            break;
        }

        // Esperar respuesta o posible END
        if (!leer_linea_fifo(fpResp, linea, sizeof(linea))) {
            fprintf(stderr, "No se pudo leer respuesta del controlador.\n");
            break;
        }

        if (strncmp(linea, "END|FIN_SIMULACION", 18) == 0) {
            printf("Agente %s termina (fin de simulación).\n", cfg.nombre);
            fclose(fpCSV);
            close(fdCtrl);
            fclose(fpResp);
            unlink(cfg.fifoRespuesta);
            return EXIT_SUCCESS;
        }

        imprimir_respuesta(linea);
        sleep(2);
    }

    fclose(fpCSV);

    // Esperar mensaje de fin de simulación
    while (leer_linea_fifo(fpResp, linea, sizeof(linea))) {
        if (strncmp(linea, "END|FIN_SIMULACION", 18) == 0) {
            printf("Agente %s termina (fin de simulación).\n", cfg.nombre);
            break;
        } else {
            // Podrían llegar respuestas pendientes si el archivo terminó antes.
            imprimir_respuesta(linea);
        }
    }

    close(fdCtrl);
    fclose(fpResp);
    unlink(cfg.fifoRespuesta);

    return EXIT_SUCCESS;
}


