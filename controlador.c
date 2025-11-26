#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#define MIN_HOUR 7
#define MAX_HOUR 19
#define MAX_NAME_LEN 64
#define MAX_FAMILY_LEN 64
#define MAX_LINE_LEN 256
#define MAX_AGENTS 64

typedef struct Reservation {
    char family[MAX_FAMILY_LEN];
    int people;
    int startHour; // inclusiva
    int endHour;   // exclusiva (startHour + 2)
} Reservation;

typedef struct ResNode {
    Reservation res;
    struct ResNode *next;
} ResNode;

typedef struct {
    char name[MAX_NAME_LEN];
    char fifoPath[128];
} AgentInfo;

// Estado global de la simulaciaIn
static int horaIni = 7;
static int horaFin = 19;
static int segHoras = 1;
static int aforoMaximo = 0;
static char pipeRecibePath[128] = {0};

static int horaActual = 7;
static int simulacionTerminada = 0;

// EstadaAsticas
static int personasPorHora[24 + 1]; // aAndice 1-24, usamos 7-19
static int solicitudesNegadas = 0;
static int solicitudesAceptadasExactas = 0;
static int solicitudesReprogramadas = 0;

// Eventos de entrada/salida por hora
static ResNode *entradasPorHora[24 + 3]; // un poco maes para salidas hasta hora+2
static ResNode *salidasPorHora[24 + 3];

// Agentes registrados
static AgentInfo agentes[MAX_AGENTS];
static int numAgentes = 0;

// SincronizaciaIn
static pthread_mutex_t mutexDatos = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

static void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }
}

static void agregar_reserva_eventos(const Reservation *r) {
    ResNode *nEntrada = (ResNode *)malloc(sizeof(ResNode));
    ResNode *nSalida = (ResNode *)malloc(sizeof(ResNode));
    if (!nEntrada || !nSalida) {
        perror("malloc");
        free(nEntrada);
        free(nSalida);
        return;
    }
    nEntrada->res = *r;
    nEntrada->next = entradasPorHora[r->startHour];
    entradasPorHora[r->startHour] = nEntrada;

    nSalida->res = *r;
    nSalida->next = salidasPorHora[r->endHour];
    salidasPorHora[r->endHour] = nSalida;
}

static AgentInfo *buscar_agente(const char *nombre) {
    for (int i = 0; i < numAgentes; ++i) {
        if (strcmp(agentes[i].name, nombre) == 0) {
            return &agentes[i];
        }
    }
    return NULL;
}

static AgentInfo *registrar_agente(const char *nombre, const char *fifoPath) {
    AgentInfo *a = buscar_agente(nombre);
    if (a) {
        // Actualizar ruta en caso de que cambie
        strncpy(a->fifoPath, fifoPath, sizeof(a->fifoPath) - 1);
        a->fifoPath[sizeof(a->fifoPath) - 1] = '\0';
        return a;
    }
    if (numAgentes >= MAX_AGENTS) {
        fprintf(stderr, "Se alcanzaI el maeximo de agentes registrados.\n");
        return NULL;
    }
    AgentInfo *nuevo = &agentes[numAgentes++];
    strncpy(nuevo->name, nombre, sizeof(nuevo->name) - 1);
    nuevo->name[sizeof(nuevo->name) - 1] = '\0';
    strncpy(nuevo->fifoPath, fifoPath, sizeof(nuevo->fifoPath) - 1);
    nuevo->fifoPath[sizeof(nuevo->fifoPath) - 1] = '\0';
    return nuevo;
}

static void enviar_mensaje_agente(const AgentInfo *ag, const char *mensaje) {
    if (!ag || !mensaje) return;
    int fd = open(ag->fifoPath, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "No se pudo abrir FIFO de agente %s (%s): %s\n",
                ag->name, ag->fifoPath, strerror(errno));
        return;
    }
    write(fd, mensaje, strlen(mensaje));
    write(fd, "\n", 1);
    close(fd);
}

// ---------------------------------------------------------------------------
// LaIgica de reservas
// ---------------------------------------------------------------------------

static int hay_cupo_bloque(int horaInicio, int personas) {
    int h1 = horaInicio;
    int h2 = horaInicio + 1;
    if (h1 < MIN_HOUR || h2 > MAX_HOUR) return 0;
    if (personasPorHora[h1] + personas > aforoMaximo) return 0;
    if (personasPorHora[h2] + personas > aforoMaximo) return 0;
    return 1;
}

// Devuelve horaInicio si encuentra espacio, -1 en caso contrario
static int buscar_bloque_alternativo(int personas) {
    for (int h = horaActual; h <= horaFin - 1; ++h) {
        if (hay_cupo_bloque(h, personas)) {
            return h;
        }
    }
    return -1;
}

static void procesar_solicitud_reserva(const char *nombreAgente,
                                       const char *familia,
                                       int horaSolicitada,
                                       int personas) {
    pthread_mutex_lock(&mutexDatos);

    AgentInfo *ag = buscar_agente(nombreAgente);
    if (!ag) {
        fprintf(stderr, "Solicitud de agente no registrado: %s\n", nombreAgente);
        pthread_mutex_unlock(&mutexDatos);
        return;
    }

    printf("PeticiaIn recibida de agente=%s familia=%s hora=%d personas=%d\n",
           nombreAgente, familia, horaSolicitada, personas);

    char respuesta[256];

    if (personas <= 0 || personas > aforoMaximo ||
        horaSolicitada < MIN_HOUR || horaSolicitada > MAX_HOUR ||
        horaSolicitada + 1 > horaFin) {
        solicitudesNegadas++;
        snprintf(respuesta, sizeof(respuesta),
                 "RESP|NEG|%s|0|0", familia);
        enviar_mensaje_agente(ag, respuesta);
        pthread_mutex_unlock(&mutexDatos);
        return;
    }

    int esExtemporanea = horaSolicitada < horaActual;

    if (!esExtemporanea && hay_cupo_bloque(horaSolicitada, personas)) {
        // Reserva en la hora solicitada
        Reservation r;
        strncpy(r.family, familia, sizeof(r.family) - 1);
        r.family[sizeof(r.family) - 1] = '\0';
        r.people = personas;
        r.startHour = horaSolicitada;
        r.endHour = horaSolicitada + 2;

        personasPorHora[horaSolicitada] += personas;
        personasPorHora[horaSolicitada + 1] += personas;
        agregar_reserva_eventos(&r);

        solicitudesAceptadasExactas++;
        snprintf(respuesta, sizeof(respuesta),
                 "RESP|OK|%s|%d|%d",
                 familia, r.startHour, r.endHour);
        enviar_mensaje_agente(ag, respuesta);
        pthread_mutex_unlock(&mutexDatos);
        return;
    }

    // Buscar bloque alternativo (para extemporaeneas o sin cupo en la hora pedida)
    int horaAlt = buscar_bloque_alternativo(personas);
    if (horaAlt != -1) {
        Reservation r;
        strncpy(r.family, familia, sizeof(r.family) - 1);
        r.family[sizeof(r.family) - 1] = '\0';
        r.people = personas;
        r.startHour = horaAlt;
        r.endHour = horaAlt + 2;

        personasPorHora[horaAlt] += personas;
        personasPorHora[horaAlt + 1] += personas;
        agregar_reserva_eventos(&r);

        solicitudesReprogramadas++;
        snprintf(respuesta, sizeof(respuesta),
                 "RESP|REPROG|%s|%d|%d",
                 familia, r.startHour, r.endHour);
        enviar_mensaje_agente(ag, respuesta);
        pthread_mutex_unlock(&mutexDatos);
        return;
    }

    // No se encontraI ningaUn bloque
    solicitudesNegadas++;
    if (esExtemporanea) {
        snprintf(respuesta, sizeof(respuesta),
                 "RESP|NEG_EXTEMP|%s|0|0", familia);
    } else {
        snprintf(respuesta, sizeof(respuesta),
                 "RESP|NEG|%s|0|0", familia);
    }
    enviar_mensaje_agente(ag, respuesta);

    pthread_mutex_unlock(&mutexDatos);
}

// ---------------------------------------------------------------------------
// Hilo de reloj
// ---------------------------------------------------------------------------

static void imprimir_eventos_hora(int hora) {
    ResNode *n;
    int salen = 0;
    int entran = 0;

    n = salidasPorHora[hora];
    while (n) {
        printf("  Familia %s sale del parque (%d personas)\n",
               n->res.family, n->res.people);
        salen += n->res.people;
        n = n->next;
    }

    n = entradasPorHora[hora];
    while (n) {
        printf("  Familia %s entra al parque (%d personas)\n",
               n->res.family, n->res.people);
        entran += n->res.people;
        n = n->next;
    }

    if (salen == 0 && entran == 0) {
        printf("  No hay cambios de familias en esta hora.\n");
    }
}

static void *hilo_reloj(void *arg) {
    (void)arg;

    for (int h = horaIni + 1; h <= horaFin; ++h) {
        sleep(segHoras);
        pthread_mutex_lock(&mutexDatos);
        horaActual = h;
        printf("\n=== Ha transcurrido una hora, son las %d hr ===\n", horaActual);
        imprimir_eventos_hora(horaActual);
        pthread_mutex_unlock(&mutexDatos);
    }

    pthread_mutex_lock(&mutexDatos);
    simulacionTerminada = 1;
    pthread_mutex_unlock(&mutexDatos);
    return NULL;
}

// ---------------------------------------------------------------------------
// Reporte final
// ---------------------------------------------------------------------------

static void imprimir_reporte_final(void) {
    printf("\n===== REPORTE FINAL DEL CONTROLADOR =====\n");

    int maxPersonas = -1;
    int minPersonas = 1e9;

    for (int h = horaIni; h <= horaFin; ++h) {
        if (personasPorHora[h] > maxPersonas) {
            maxPersonas = personasPorHora[h];
        }
        if (personasPorHora[h] < minPersonas) {
            minPersonas = personasPorHora[h];
        }
    }

    printf("Horas pico (mayor ocupaciaIn = %d personas): ", maxPersonas);
    for (int h = horaIni; h <= horaFin; ++h) {
        if (personasPorHora[h] == maxPersonas) {
            printf("%d ", h);
        }
    }
    printf("\n");

    printf("Horas de menor ocupaciaIn (=%d personas): ", minPersonas);
    for (int h = horaIni; h <= horaFin; ++h) {
        if (personasPorHora[h] == minPersonas) {
            printf("%d ", h);
        }
    }
    printf("\n");

    printf("Solicitudes negadas: %d\n", solicitudesNegadas);
    printf("Solicitudes aceptadas en su hora: %d\n", solicitudesAceptadasExactas);
    printf("Solicitudes reprogramadas: %d\n", solicitudesReprogramadas);
}

static void notificar_fin_a_agentes(void) {
    for (int i = 0; i < numAgentes; ++i) {
        enviar_mensaje_agente(&agentes[i], "END|FIN_SIMULACION");
    }
}

// ---------------------------------------------------------------------------
// Parseo de argumentos
// ---------------------------------------------------------------------------

static void uso(const char *prog) {
    fprintf(stderr,
            "Uso: %s -i horaIni -f horaFin -s segHoras -t total -p pipeRecibe\n",
            prog);
}

static int parse_args(int argc, char *argv[]) {
    int opt;
    int got_i = 0, got_f = 0, got_s = 0, got_t = 0, got_p = 0;

    while ((opt = getopt(argc, argv, "i:f:s:t:p:")) != -1) {
        switch (opt) {
            case 'i':
                horaIni = atoi(optarg);
                got_i = 1;
                break;
            case 'f':
                horaFin = atoi(optarg);
                got_f = 1;
                break;
            case 's':
                segHoras = atoi(optarg);
                got_s = 1;
                break;
            case 't':
                aforoMaximo = atoi(optarg);
                got_t = 1;
                break;
            case 'p':
                strncpy(pipeRecibePath, optarg, sizeof(pipeRecibePath) - 1);
                pipeRecibePath[sizeof(pipeRecibePath) - 1] = '\0';
                got_p = 1;
                break;
            default:
                uso(argv[0]);
                return -1;
        }
    }

    if (!got_i || !got_f || !got_s || !got_t || !got_p) {
        uso(argv[0]);
        return -1;
    }
    if (horaIni < MIN_HOUR || horaIni > MAX_HOUR ||
        horaFin < MIN_HOUR || horaFin > MAX_HOUR ||
        horaIni >= horaFin) {
        fprintf(stderr, "Rango de horas invaelido. Debe estar entre %d y %d y horaIni<horaFin.\n",
                MIN_HOUR, MAX_HOUR);
        return -1;
    }
    if (segHoras <= 0 || aforoMaximo <= 0) {
        fprintf(stderr, "segHoras y total (aforo) deben ser > 0.\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Bucle principal de recepciaIn
// ---------------------------------------------------------------------------

static void manejar_linea_mensaje(char *linea) {
    trim_newline(linea);
    if (linea[0] == '\0') return;

    char tipo[16];
    char *rest = NULL;

    char *token = strtok_r(linea, "|", &rest);
    if (!token) return;
    strncpy(tipo, token, sizeof(tipo) - 1);
    tipo[sizeof(tipo) - 1] = '\0';

    if (strcmp(tipo, "REG") == 0) {
        // REG|nombreAgente|fifoRespuesta
        char *nombreAgente = strtok_r(NULL, "|", &rest);
        char *fifoResp = strtok_r(NULL, "|", &rest);
        if (!nombreAgente || !fifoResp) {
            fprintf(stderr, "Mensaje REG mal formado.\n");
            return;
        }
        pthread_mutex_lock(&mutexDatos);
        AgentInfo *ag = registrar_agente(nombreAgente, fifoResp);
        if (ag) {
            char msg[64];
            snprintf(msg, sizeof(msg), "TIME|%d", horaActual);
            enviar_mensaje_agente(ag, msg);
            printf("Agente registrado: %s (FIFO=%s)\n", ag->name, ag->fifoPath);
        }
        pthread_mutex_unlock(&mutexDatos);
    } else if (strcmp(tipo, "REQ") == 0) {
        // REQ|nombreAgente|familia|hora|personas
        char *nombreAgente = strtok_r(NULL, "|", &rest);
        char *familia = strtok_r(NULL, "|", &rest);
        char *horaStr = strtok_r(NULL, "|", &rest);
        char *persStr = strtok_r(NULL, "|", &rest);
        if (!nombreAgente || !familia || !horaStr || !persStr) {
            fprintf(stderr, "Mensaje REQ mal formado.\n");
            return;
        }
        int hora = atoi(horaStr);
        int personas = atoi(persStr);
        procesar_solicitud_reserva(nombreAgente, familia, hora, personas);
    } else {
        fprintf(stderr, "Tipo de mensaje desconocido: %s\n", tipo);
    }
}

int main(int argc, char *argv[]) {
    if (parse_args(argc, argv) != 0) {
        return EXIT_FAILURE;
    }

    horaActual = horaIni;
    printf("Controlador iniciado. SimulaciaIn de %d a %d, aforo=%d, segHoras=%d\n",
           horaIni, horaFin, aforoMaximo, segHoras);

    // Crear FIFO principal si no existe
    if (mkfifo(pipeRecibePath, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo");
            return EXIT_FAILURE;
        }
    }

    int fdRead = open(pipeRecibePath, O_RDONLY);
    if (fdRead == -1) {
        perror("open pipeRecibe (lectura)");
        return EXIT_FAILURE;
    }
    // Mantener un descriptor de escritura abierto para que read no devuelva EOF
    int fdDummyWrite = open(pipeRecibePath, O_WRONLY);
    if (fdDummyWrite == -1) {
        perror("open pipeRecibe (dummy escritura)");
        close(fdRead);
        return EXIT_FAILURE;
    }

    FILE *fp = fdopen(fdRead, "r");
    if (!fp) {
        perror("fdopen");
        close(fdRead);
        close(fdDummyWrite);
        return EXIT_FAILURE;
    }

    pthread_t thrReloj;
    if (pthread_create(&thrReloj, NULL, hilo_reloj, NULL) != 0) {
        perror("pthread_create");
        fclose(fp);
        close(fdDummyWrite);
        return EXIT_FAILURE;
    }

    char linea[MAX_LINE_LEN];
    while (1) {
        if (!fgets(linea, sizeof(linea), fp)) {
            if (feof(fp)) {
                clearerr(fp);
                continue;
            } else {
                perror("fgets");
                break;
            }
        }
        manejar_linea_mensaje(linea);

        pthread_mutex_lock(&mutexDatos);
        int fin = simulacionTerminada;
        pthread_mutex_unlock(&mutexDatos);
        if (fin) {
            break;
        }
    }

    pthread_join(thrReloj, NULL);

    notificar_fin_a_agentes();
    imprimir_reporte_final();

    fclose(fp);
    close(fdDummyWrite);

    return EXIT_SUCCESS;
}


