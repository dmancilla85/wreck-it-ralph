#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <semaphore.h>
#include <fcntl.h>
#include <error.h>
#include <list>
#include <iterator>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <algorithm>

#include <SDL/SDL.h>
#include "/usr/include/SDL/SDL_image.h"
#include "SDL/SDL_ttf.h"
#include <X11/Xlib.h>

#define BACKLOG 100 /* El número de conexiones permitidas */
#define TAMBUF 255
#define LIBRE 0
#define OCUPADO 1
#define DESCONECTADO -1
#define CONFIGFILEPATH "ServerConfig.cfg"
#define EVERYTHING_OK 1234

#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT  600
#define SCREEN_BPP  32
#define CAPACIDAD_CONSOLA  22

using namespace std;
int inscripcionAbierta=1;

typedef struct
{
    //Estructura de clientes
    int orden;
    int socket;
    char nombre[255];
    int estado;
    int puntaje;
    int cantVidas;
    int cantPartidasGanadas;
}  Jugador;

typedef struct
{
    //Estructura configuracional del servidor.
    int tiempoInmunidad;
    int tiempoTorneo;
    int puerto;
}  ServerConfig;

typedef struct
{
    //Estructura de partida.
    Jugador jugador1;
    Jugador jugador2;
    int tiempoInmunidad;
    int ganador;
    int puntaje1;
    int puntaje2;
    pid_t pid;
}  Partida;


void inicializarMemoriaCompartida();
void inicializarConexion();
void * aceptarConexiones(void *);
void cargarConfiguracion();
void lanzarPartidas();
vector<Jugador> getJugadoresActivos();
int yaJugaron(int, int);
int estanLibres(int, int);
int estaVivo(int);
void eliminarJugador(int);
void ocuparJugadores(int, int);
static void finalizarServidor(int);
static void finalizarServerPartidaHandler(int);
void actualizarJugadores(Partida);
void borrarPartidaActiva(Partida *);
void imprimirPuntajes();
int cantidadJugadoresActivos();
char * obtenerGanador();
void finalizarTorneo();
void finalizarSemaforoStart(pid_t);
void *vivo(void *);
void exitError();
void *capturar_teclado (void *descriptor);

//Parte grafica
void printF(string);
void imprimirPuntajes(vector <string>);
void imprimirLista(vector <string>, int);
void FillRect(int , int , int , int , int );
void clean_up();
bool load_files();
bool init();

SDL_Surface *load_image( std::string filename );
void apply_surface( int x, int y, SDL_Surface* source, SDL_Surface* destination, SDL_Rect* clip);
void * imprimirTiempo(void *);

vector<Jugador> jugadoresList; //Vector principal donde están guardados los datos de los clientes.
vector<vector<int> > partidasJugadas;
vector<Partida> partidasList; //Vector principal donde están guardados los datos de los clientes.
int socketFD; //File descriptor del socket principal.

Partida * partidaMC;
time_t *tiempoMC;
pthread_t aceptarConexiones_tid, estoyVivo_tid, imprimirTiempo_tid, teclado_tid;
bool aceptarConexionesTSet=false, estoyVivoTSet=false;

char semEscrituraName[30]="/semEscritura";
char semLecturaName[30]="/semLectura";
char semLiveName[30]="/semLive";
char shmName[30]="/shMemory";
char shmNameLive[30]="/shMemoryLive";

sem_t * semEscritura;
sem_t * semLectura;
sem_t * semLive;


ServerConfig serverConfig;

SDL_Surface *background = NULL, *message = NULL, *screen = NULL;
SDL_Color textColor = { 0, 0, 0};
vector<string> messageList;
TTF_Font *font = NULL;

int main(int argc, char *argv[])
{
    signal(SIGINT, finalizarServidor);
    signal(SIGKILL, finalizarServidor);
    signal(SIGTERM, finalizarServidor);
    signal(SIGUSR1, finalizarServidor);
    signal(SIGCHLD, finalizarServerPartidaHandler);

    //Inicializamos el servidor.
    if ( init() == false )
        return 1;
    if ( load_files() == false )
        return 1;

    atexit(SDL_Quit);

    apply_surface( 0, 0, background, screen, NULL);
    SDL_Flip(screen);
    printF("Inicializando torneo");
    imprimirPuntajes();
    inicializarMemoriaCompartida();
    pthread_create(&estoyVivo_tid, NULL, vivo, NULL);
    pthread_create(&teclado_tid, NULL, capturar_teclado, NULL);
    cargarConfiguracion();
    inicializarConexion();
    lanzarPartidas();

    while (1) {}
    return EXIT_SUCCESS;
}
void *vivo(void *p)
{
    estoyVivoTSet=true;
    while (1)
    {
        sem_wait(semLive);
        *tiempoMC = time(NULL);
        sem_post(semLive);
        sleep(3);
    }
}
void inicializarMemoriaCompartida()
{
    printF("Inicializando memoria compartida");
    fflush(stdout);
    int shmId, shmIdLive ; //File descriptor de la memoria compartida.
    //Inicializo la memoria compartida y los semáforos correspondientes.
    int aInt = getpid();
    char pid[6];
    sprintf(pid, "%d", aInt);
    strcat(shmName, pid);
    strcat(shmNameLive, pid);

    //Memoria para intercambiar datos con las partidas
    if ( ( shmId = shm_open(shmName, O_CREAT | O_RDWR, 0777 ) ) < 0 )
    {
        printF("Error creando la memoria compartida.");
        fflush(stdout);
        exit(1);
    }

    if (ftruncate(shmId, sizeof(Partida)) == -1)
    {
        printF("Error estableciendo el tamaño. ");
        exitError();

    }

    partidaMC = (Partida *) mmap(0, sizeof(Partida), PROT_READ | PROT_WRITE, MAP_SHARED, shmId, 0);

    if (partidaMC == MAP_FAILED)
    {
        printF("Error mapeando a memoria.");
        exitError();
    }

    //memoria para que las partidas verifiquen que el torneo esta vivo
    if ( ( shmIdLive = shm_open(shmNameLive, O_CREAT | O_RDWR, 0777 ) ) < 0 )
    {
        printF("Error creando la memoria compartida vivo.");
        exitError();
    }
    if (ftruncate(shmIdLive, sizeof(time_t)) == -1)
    {
        printF("Error estableciendo el tamaño memoria vivo.");
        exitError();
    }
    tiempoMC = (time_t *) mmap(0, sizeof(time_t), PROT_READ | PROT_WRITE, MAP_SHARED, shmIdLive, 0);
    if (tiempoMC == MAP_FAILED)
    {
        printF("Error mapeando a memoria vivo.");
        exitError();
    }

    // Se crean semáforos para MC
    strcat(semEscrituraName, pid);
    strcat(semLecturaName, pid);
    strcat(semLiveName, pid);

    semEscritura = sem_open( semEscrituraName, O_CREAT | O_EXCL , 0777, 1);
    semLectura = sem_open( semLecturaName, O_CREAT | O_EXCL , 0777, 1);
    semLive = sem_open( semLiveName, O_CREAT | O_EXCL , 0777, 1);

}
void exitError()
{
    printF("Saliendo en 5 segundos..");
    sleep(5);
    finalizarServidor(1);
}
void inicializarConexion()
{
    printF("Inicializando conexion");
    //Método que inicializa el listen en el server, y lanza el thread que aceptar las conexiones.
    int yes=1;
    struct sockaddr_in server;
    struct sockaddr_in client;

    int reuse;
    int sin_size;

    int auxFd, i;
    //Creamos el socket de escucha.
    if ((socketFD=socket(AF_INET, SOCK_STREAM, 0)) == -1 )
    {
        printF("Error en socket!");
        exitError();
    }

    //Hacemos que sea reutilizable, para los casos en los que se cierra el servidor y
    // se quiera reutilizar el socket antes de que sea completamente reseteado por el "SO" (Al cerrarlo queda en un estado intermedio)
    if (setsockopt(socketFD, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
        printF("Setsockopt error!");
        exitError();
    }

    //Inicializamos el server y bindeamos.
    server.sin_family = AF_INET;
    server.sin_port = htons(serverConfig.puerto);
    //printf("Escuchando en el puerto %d\n", serverConfig.puerto);
    fflush(stdout);
    server.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY coloca nuestra dirección IP automáticamente
    bzero(&(server.sin_zero),8);

    if (bind(socketFD,(struct sockaddr*)&server,sizeof(struct sockaddr))==-1)
    {
        printF("Error en bind");
        exitError();
    }

    if (listen(socketFD,BACKLOG) == -1)
    {
        printF("Error en listen");
        exitError();
    }
    pthread_create(&aceptarConexiones_tid, NULL, aceptarConexiones, NULL);
}
void * aceptarConexiones(void * p)
{
    printF("Aceptando conexiones");
    aceptarConexionesTSet=true;
    //Thread que acepta constantemente e inicializa usuarios.
    Jugador jugador;
    int auxFd;
    int sin_size;
    struct sockaddr_in client;
    char mensaje[255];

    while (1)
    {
        //Loopeamos infinitamente aceptando conexiones
        sin_size=sizeof(struct sockaddr_in);
        if ((auxFd = accept(socketFD,(struct sockaddr *)&client,(socklen_t *) &sin_size))==-1)
        {
            printf("Ha ocurrido un error al intentar aceptar una petición de conexión.\n");
            exitError();
        }

        while ((recv(auxFd, mensaje, 255, 0)) <= 0) {}

        strcpy(jugador.nombre, mensaje);
        char buffer[255];
        sprintf(buffer, "Se conecto %s", jugador.nombre);
        printF(string(buffer));
        fflush(stdout);
        //Agregamos un nuevo cliente, seteamos su socket y todos los datos necesarios.
        jugador.socket = auxFd;
        jugador.puntaje = 0;
        jugador.estado = LIBRE;
        jugador.orden = jugadoresList.size();
        jugador.cantVidas = 3;
        jugadoresList.push_back(jugador);
        partidasJugadas.push_back(vector<int>());
        imprimirPuntajes();
    }
    printF("No se aceptan mas conexiones\n");
    fflush(stdout);
}
void * imprimirTiempo(void * p)
{
    int tiempoTotal = serverConfig.tiempoTorneo;
    int ancho = 170, alto=20, inicioX=20, inicioY=(SCREEN_HEIGHT - 50);
    char textoLoco [200];

    SDL_Rect destinoRect;
    destinoRect.x=inicioX;
    destinoRect.y=inicioY;
    destinoRect.w=ancho;
    destinoRect.h=alto;
    SDL_Color colores = {255,255,255};
    SDL_Surface *text;
    while (tiempoTotal>0)
    {
        tiempoTotal--;

        if (tiempoTotal==30)
        {
            colores.r=255;
            colores.g=0;
            colores.b=0;
        }

        if (SDL_FillRect(screen, &destinoRect, SDL_MapRGB( screen->format, 0,0,0))!= 0)
        {
         break;
        }

        sprintf(textoLoco, "Tiempo restante: %d:%d", tiempoTotal/60, tiempoTotal%60);
        //printf("Texto: %s\n", textoLoco);
        text = TTF_RenderText_Solid(font, textoLoco, colores);

        SDL_BlitSurface(text, NULL, screen, &destinoRect);
        //SDL_FreeSurface(text);
        SDL_Flip(screen);
         //SDL_LockSurface(screen);
        SDL_UpdateRect(screen, inicioX, inicioY, ancho, alto);
        //SDL_UnlockSurface(screen);
        //SDL_FreeSurface(text);
        //SDL_Delay(1000);
        sleep(1);
    }
}
void cargarConfiguracion()
{
    printF("Cargando configuracion");
    fflush(stdout);
    //Leo la configuración en el formato "key value" y cargo la estructura.
    FILE * fpConfiguracion = fopen(CONFIGFILEPATH, "r");
    if (!fpConfiguracion)
    {
        printF("No hay configuracion, abortando\n");
        exitError();
    }
    char key[50];
    char value[50];
    while (!feof(fpConfiguracion))
    {
        fscanf(fpConfiguracion, "%s %s", key, value);
        if (!strcmp(key, "tiempoInmunidad"))
            serverConfig.tiempoInmunidad = atoi(value);
        else if (!strcmp(key, "puerto"))
            serverConfig.puerto = atoi(value);
        else if (!strcmp(key, "tiempoTorneo"))
            serverConfig.tiempoTorneo = atoi(value);
    }
    fclose(fpConfiguracion);
}

void lanzarPartidas()
{
    printF("Lanzando partidas");
    fflush(stdout);
    Partida partidaNueva;
    int i, j;
    time_t tiempoInicio;
    while (cantidadJugadoresActivos() < 2)
    {
        SDL_Flip(screen);
        sleep(2);
    }

    pthread_create(&imprimirTiempo_tid, NULL, imprimirTiempo, NULL);
    printF("Comenzando torneo!");
    tiempoInicio = time(NULL);

    while (difftime(time(NULL), tiempoInicio) <= serverConfig.tiempoTorneo)
    {
        SDL_Flip(screen);
        sleep(2);
        //Itero la lista dos veces, todos contra todos
        for (i = 0; i < jugadoresList.size(); i++)
        {
            if (!jugadoresList[i].estado==LIBRE) continue;
            for (j = 0; j < jugadoresList.size(); j++)
            {
                if (i==j || !jugadoresList[j].estado==LIBRE) continue; //Si están desconectados o estoy sobre el mismo jugador, continuo.

                if ( !yaJugaron(i,j)) // Si no jugaron una partida y no estan ocupados
                {
                    char buffer[255];
                    sprintf(buffer,"Lanzando una nueva partida entre %s y %s", jugadoresList[i].nombre, jugadoresList[j].nombre);
                    printF(string(buffer));
                    fflush(stdout);
                    partidaNueva.jugador1 = jugadoresList[i];
                    partidaNueva.jugador2 = jugadoresList[j];
                    partidaNueva.puntaje1 = partidaNueva.puntaje2 = 0;
                    partidaNueva.jugador1.cantVidas = 3;
                    partidaNueva.jugador2.cantVidas = 3;
                    partidaNueva.tiempoInmunidad = serverConfig.tiempoInmunidad;
                    partidaNueva.ganador = -1; //No hay ganador
                    ocuparJugadores(i, j);

                    sem_wait(semEscritura); //Una vez que bloqueo este semáforo,
                    //es importante entender que hasta
                    // que el server de partida no lo libere
                    // rescatando sus datos iniciales, no se va a poder crear otra partida.
                    partidaNueva.pid = -1;
                    *partidaMC = partidaNueva;

                    partidaNueva.pid = fork();

                    if ( partidaNueva.pid == 0 )
                    {
                        execv("./Partida", NULL);
                    }
                    else
                    {
                        partidasList.push_back(partidaNueva);
                        imprimirPuntajes();
                        break;
                    }
                }

            }
        }
    }
    printF("Se acabo el tiempo del torneo!!");
    if (aceptarConexionesTSet)
    {
        pthread_cancel(aceptarConexiones_tid);
        aceptarConexionesTSet=false;
    }
    close(socketFD);
    printF("No se aceptan mas conexiones");
    while (partidasList.size()) sleep(5);
    finalizarTorneo();
}
int cantidadJugadoresActivos()
{
    int cant=0;
    for (int i=0; i<jugadoresList.size(); i++)
        cant+= (jugadoresList[i].estado != DESCONECTADO);
    return cant;
}
vector<Jugador> getJugadoresActivos()
{
    //Método que devuelve los jugadores activos, no sé si lo vamos a usar.
    int i;
    vector<Jugador> activos;
    for (i=0; i<jugadoresList.size(); i++)
        if (!jugadoresList[i].estado == DESCONECTADO) activos.push_back(jugadoresList[i]);
    return activos;
}
int yaJugaron(int orden1, int orden2)
{
    //Se fija si el jugador 2 están en la lista de partidas jugadas del jugador 1
    for (int i=0; i<partidasJugadas[orden1].size(); i++)
        if (partidasJugadas[orden1][i] == orden2) return 1;
    return 0;
}

int estanLibres(int orden1, int orden2)
{
    //Se fija si ambos jugadores están libres.
    return (jugadoresList[orden1].estado == LIBRE && jugadoresList[orden2].estado == LIBRE);
}

int estaVivo(int orden)
{
    int socket = jugadoresList[orden].socket;
    char buffer[32];
    if ( recv(socket, buffer, sizeof(buffer), MSG_PEEK | MSG_DONTWAIT) == 0 )
    {
        eliminarJugador(orden);
        return 0;
    }
    return 1;
}

void eliminarJugador(int orden)
{
    char buffer[255];
    sprintf(buffer, "%s se ha desconectado", jugadoresList[orden].nombre);
    printF(string(buffer));
    jugadoresList[orden].estado = DESCONECTADO;
    close(jugadoresList[orden].socket);
    imprimirPuntajes();
}

void ocuparJugadores(int orden1, int orden2)
{
    //Los pongo ocupados y les agrego el indice del otro a la lista de partidas jugadas.
    jugadoresList[orden1].estado = OCUPADO;
    jugadoresList[orden2].estado = OCUPADO;
    partidasJugadas[orden1].push_back(orden2);
    partidasJugadas[orden2].push_back(orden1);
    //printf("Terminando de ocupar jugadores\n");
    fflush(stdout);
}

void finalizarTorneo()
{
    char buffer[255];
    sprintf(buffer,"Ha concluido el torneo!, el ganador es %s", obtenerGanador());
    printF(string(buffer));

    char message[255];
    sprintf(message, "ft %s", obtenerGanador());
    for (int i=0; i<jugadoresList.size(); i++)
        if (jugadoresList[i].estado != DESCONECTADO)
            send(jugadoresList[i].socket, message , TAMBUF, 0);
    printF("Este servidor se autodestruira en 20 segundos!");
    sleep(20);
    finalizarServidor(EVERYTHING_OK);
}
char * obtenerGanador()
{
    int posMax = 0;
    bool hayEmpate = true;
    for (int i=1; i<jugadoresList.size(); i++)
    {
        hayEmpate = (jugadoresList[i].puntaje == jugadoresList[posMax].puntaje);
        posMax = jugadoresList[i].puntaje > jugadoresList[posMax].puntaje? i:posMax;
    }
    return  strdup((hayEmpate?"EMPATE":jugadoresList[posMax].nombre));
}
static void finalizarServidor(int signo)
{
    //Cancela todos los hilos, envía el mensaje de finalización a cada cliente,
    // y cierra el socket, espera a los hilos con pthread_exit.
    for (int i=0; i<jugadoresList.size(); i++)
        if (jugadoresList[i].estado != DESCONECTADO)
        {
            send(jugadoresList[i].socket, "END" , TAMBUF, 0);
            close(jugadoresList[i].socket);
        }


    if (signo!=EVERYTHING_OK && aceptarConexionesTSet)
    {
        pthread_cancel(aceptarConexiones_tid);
        aceptarConexionesTSet=false;
    }
    if (estoyVivoTSet)
    {
        pthread_cancel(estoyVivo_tid);
        estoyVivoTSet=false;
        //pthread_cancel(teclado_tid);
        //pthread_cancel(imprimirTiempo_tid );
    }


    for (int i=0; i<partidasList.size(); i++)
        kill(partidasList[i].pid, SIGTERM);

    munmap(partidaMC, sizeof(Partida));
    shm_unlink(shmName);
    munmap(tiempoMC, sizeof(Partida));
    shm_unlink(shmNameLive);


    sem_close(semLectura);
    sem_close(semEscritura);
    sem_close(semLive);
    sem_unlink(semLecturaName);
    sem_unlink(semEscrituraName);
    sem_unlink(semLiveName);
    TTF_Quit;
    SDL_Quit;
    exit(0);
}
static void finalizarServerPartidaHandler(int signo)
{


    Partida partidaFinalizada;
    //sem_wait(semLectura);
    partidaFinalizada = *partidaMC;
    //printf("PID: %d\n", partidaFinalizada.pid);
    fflush(stdout);
    if (partidaFinalizada.ganador == -1) //Si terminó abruptamente, es probable
        // que en memoria compartida no tenga el PID, ya que el pid lo setea el proceso de partida, entonces hago waitpid.
    {
        pid_t pid;
        int   status;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            finalizarSemaforoStart(pid);   // Or whatever you need to do with the PID
    }
    else wait(NULL);
    sem_post(semEscritura); //Una vez que leí lo que el servidor de partida escribió, permito escribir a los demás.
    char buffer[255];
    printf("Finalizo partida, llego SIGCHLD\n");
    fflush(stdout);
    sprintf(buffer,"La partida entre %s y %s ha finalizado.", partidaFinalizada.jugador1.nombre, partidaFinalizada.jugador2.nombre);
    printF(string(buffer));
    fflush(stdout);
    borrarPartidaActiva(&partidaFinalizada);
    actualizarJugadores(partidaFinalizada);
    imprimirPuntajes();

}
void finalizarSemaforoStart(pid_t pid)
{
    printf("Finalizando el semaforo de start porque la partida termino abruptamente\n");
    fflush(stdout);
    char semStartName[30]="/semStart";
    char aux[10];
    sprintf(aux, "%d", pid);
    strcat(semStartName, aux);
    printf("semStartName: %s\n", semStartName);
    fflush(stdout);
    sem_close(sem_open(semStartName, O_CREAT, 0777));
    sem_unlink(semStartName);
}
void actualizarJugadores(Partida partida)
{
    // Actualizo los jugadores, en la lista va a quedar si están activos o no,
    // ya que el server de partida los setea al finalizar, y también tendrán su puntaje final.
    jugadoresList[partida.jugador1.orden] = partida.jugador1;
    jugadoresList[partida.jugador2.orden] = partida.jugador2;
    jugadoresList[partida.jugador1.orden].puntaje += partida.puntaje1;
    jugadoresList[partida.jugador2.orden].puntaje += partida.puntaje2;

    if (jugadoresList[partida.jugador1.orden].estado != DESCONECTADO)
        jugadoresList[partida.jugador1.orden].estado = LIBRE;
    else
        eliminarJugador(partida.jugador1.orden);

    if (jugadoresList[partida.jugador2.orden].estado != DESCONECTADO)
        jugadoresList[partida.jugador2.orden].estado = LIBRE;
    else
        eliminarJugador(partida.jugador2.orden);
}
void borrarPartidaActiva(Partida * p)
{
    int i;
    for (i=0; i<partidasList.size(); i++)
        if (partidasList[i].pid == p->pid)
        {
            partidasList.erase(partidasList.begin()+i);
            return;
        }
}

//Parte gráfica
void apply_surface( int x, int y, SDL_Surface* source, SDL_Surface* destination, SDL_Rect* clip)
{
    SDL_Rect offset;

    offset.x = x;
    offset.y = y;

    SDL_BlitSurface( source, clip, destination, &offset );
}

SDL_Surface *load_image( std::string filename )
{
    SDL_Surface* loadedImage = NULL;
    SDL_Surface* optimizedImage = NULL;
    loadedImage = IMG_Load( filename.c_str() );

    if ( loadedImage != NULL )
    {
        optimizedImage = SDL_DisplayFormat( loadedImage );
        SDL_FreeSurface( loadedImage );
        if ( optimizedImage != NULL )
            SDL_SetColorKey( optimizedImage, SDL_SRCCOLORKEY, SDL_MapRGB( optimizedImage->format, 0, 0xFF, 0xFF ) );
    }

    return optimizedImage;
}


bool init()
{
    XInitThreads();

    if ( SDL_Init( SDL_INIT_VIDEO ) == -1 )
        return false;

    screen = SDL_SetVideoMode( SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_BPP, SDL_SWSURFACE );

    if ( screen == NULL )
        return false;
    if ( TTF_Init() == -1 )
        return false;

    SDL_WM_SetIcon(SDL_LoadBMP("data/ralph_ico.bmp"), NULL);
    SDL_WM_SetCaption( "Fix It Felix Tournament! - Sistemas Operativos TP4", NULL );

    return true;
}

bool load_files()
{
    background = load_image( "data/img/background.png" );
    font = TTF_OpenFont( "data/img/arial.ttf", 15 );

    if ( background == NULL )
        return false;
    if ( font == NULL )
        return false;

    return true;
}


void clean_up()
{
    SDL_FreeSurface( background );
    SDL_FreeSurface( message );

    TTF_CloseFont( font );

    TTF_Quit();
    SDL_Quit();
}

void FillRect(int x, int y, int w, int h, int color)
{
    SDL_Rect rect = {x,y,w,h};
    SDL_FillRect(screen, &rect, color);
}

void imprimirLista(vector <string> messages, int posX)
{
    int posY = 65;
    FillRect(posX-5,posY,350,450,0xCCCCCC);
    for (int i=0; i<messages.size(); i++)
    {
        message = TTF_RenderText_Solid( font, messages[i].c_str(), textColor );
        apply_surface( posX, posY, message, screen, NULL);
        posY+= 20;
    }
    if ( SDL_Flip( screen ) == -1 )
        return;
}


void imprimirPuntajes()
{
    vector<string> puntajes;
    char buffer[255];
    for (int i=0; i<jugadoresList.size(); i++)
    {
        sprintf(buffer, "Jugador %s: Puntaje: %d | Estado: %s", jugadoresList[i].nombre,
                jugadoresList[i].puntaje, jugadoresList[i].estado == DESCONECTADO? "DESCONECTADO":
                jugadoresList[i].estado==LIBRE? "LIBRE":jugadoresList[i].estado==OCUPADO?"OCUPADO":"UNDEFINED");
        puntajes.push_back(buffer);
    }
    imprimirLista(puntajes, 40);
}


void printF(string msj)
{
    messageList.push_back(msj);
    if (messageList.size() > CAPACIDAD_CONSOLA) //Si llego al maximo de la capacidad de la consola
    {
        //Intercambio todos los strings, al final de este for, el que estaba primero (el mas viejo), termina en el final
        for (int i=0; i<messageList.size()-1; i++)
            messageList[i].swap(messageList[i+1]);

        messageList.pop_back(); //Le hago un pop, ya que quedo el mas viejo al final.
    }
    imprimirLista(messageList, 420); //Mando a imprimir toda la lista.
}

void *capturar_teclado (void *descriptor)
{
    sleep(1);
    SDL_Event event;
    Uint8 *keys;
    keys=SDL_GetKeyState(NULL);

    while(SDL_WaitEvent(&event))
    {
        switch(event.type)
        {
        case SDL_QUIT:
            printf("PRESIONO X\n");
            finalizarServidor(0);

            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.sym==SDLK_ESCAPE)
                printf("PRESIONO ESC\n");
            finalizarServidor(0);
            break;
        }
        usleep(300);
    }
    return NULL;
}
