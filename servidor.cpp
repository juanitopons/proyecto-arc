/**********************************************************************************************************/
/*                                                                                                        */
/*                           ~ Proyecto Arquitectura de Redes de Computadores ~                           */
/*                                                                                                        */
/*   Fichero: servidor.cpp                                             Autor: Jorge Redondo Palacios      */
/*   Licencia: GPL V2                                                                                     */
/*                                                                                                        */
/*    ----- Arquitectura del Servidor:                                                                    */
/*                                                                                                        */
/*        El servidor desarrollado se trata de un servidor multi-hilo, con sockets bloqueantes con el     */
/*    objetivo de alcanzar altas prestaciones. Se han hecho varias decisiones de diseño para alcanzar     */
/*    dicha meta, o al menos conseguir el mayor rendimiento posible.                                      */
/*                                                                                                        */
/*        En primer lugar, se ha optado por una arquitectura multi-hilo ya que se ajusta perfectamente    */
/*    al tipo de sistema a desarrollar. Puesto que los clientes se dividirán en grupos, y nunca inter-    */
/*    accionarán clientes de distintos grupos, cada hilo puede guardar en local una lista de todos sus    */
/*    clientes conectados. Aparte de que ningún hilo debería acceder a la lista de miembros de otros      */
/*    hilos (Es decir, el funcionamiento de un grupo es independiente del de los demás), restringimos     */
/*    la búsqueda de las IDs, haciendo desaparecer el coste de buscar a qué grupo pertenecia cada pa-     */
/*    quete entrante (Que tendríamos en una arquitectura de un solo hilo); búsqueda con un coste en ca-   */
/*    so peor que era lineal con el número total de clientes entre todos los grupos.                      */
/*                                                                                                        */
/*        Se ha optado por utilizar sockets bloqueantes, ya que, por la propia naturaleza del sistema,    */
/*    no se tiene la necesidad de realizar otras tareas mientras esperamos que se envíen o lean datos,    */
/*    y si se utilizasen sockets no bloqueantes, el SO mantendría constantemente el proceso activo por    */
/*    culpa del polling producido. En cambio, con los sockets bloqueantes utilizados, el proceso sólo     */
/*    es despertado cuando termina la operación, permitiendo continuar. Se hubieran podido utilizar       */
/*    sockets no bloqueantes y paralelizar el reenvío de mensajes a los miembros de un grupo, pero        */
/*    creemos que con muchos clientes obtendríamos un peor rendimiento por culpa del overhead de lanzar   */
/*    tantos hilos como clientes de un grupo, y destruirlos a los pocos milisegundos.                     */
/*                                                                                                        */
/*	  Además se ha abandonado el uso de la sentencia select() debido a su bajo rendimiento y escala-  */
/*    bilidad. En su lugar utilizamos epoll, un sistema sólo de Linux presente desde la versión del       */
/*    kernel 2.6. epoll no sólo elimina el límite que tenía select() en cuanto a total de descriptores,   */
/*    sino que además el coste de select() era lineal con el número de descriptores a comprobar, mientras */
/*    que el de epoll es lineal. Esto provoca que la aplicación sea mucho más escalable, más eficiente,   */
/*    y soporte más clientes.                                                                             */
/*                                                                                                        */
/**********************************************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <errno.h>
#include <thread>
#include <map>
#include <vector>
#include <string.h>
#include <unistd.h>
#include <mutex>
#include <iostream>

#include "mensajes.h"
#include "network.h"

#define SERVER_PORT  12345
#define MAXEVENTS	 100000

#define TRUE             1
#define FALSE            0

#define _DEBUG_



using namespace std;

/* Almacenaremos todos los hilos creados para los grupos en un vector, para, cuando terminemos, esperar a que
estos hilos terminen antes */
vector<thread> grupos_hilos;

/* Los descriptores de fichero de epoll permitirán al hilo principal actualizarlos cuando entre nuevas conexiones que
se unan a los grupos Este descriptor se le pasará al hilo para que espere sobre él. Se ha optado por un contendor de tipo
map<key,value> por su coste de búsqueda de log(n). Aún no siendo una operación crítica, sí que va a haber más accesos a
las estructuras fd_set que inserciones de nuevos grupos, por lo que interesa mantenerlo en un contendeor ordenado y con
índice binario para búsqueda */
map<grupoid_t,int> grupos_sets;


/* grupo_thread es la función que ejecutará cada hilo. Cada hilo se le asgina a un grupo y
es el único encargado de pasar todos los mensajes de los miembros del grupo, de esta forma
sólo el hilo es consciente de lo que pasa en cada grupo así de sus miembros conectados, 
aislando cada hilo las tareas de gestión de mensajes de los grupos. Es la principal herramienta
para paralelizar las tareas del servidor. */
void grupo_thread (int epoll_thread_fd) 
{
	int  								desc_ready, rc, cerrar_hilo = FALSE, socket;
	struct epoll_event* 				events;
	struct mensaje_saludo 				saludo;
	struct mensaje_posicion 			posicion;
	struct mensaje_reconocimiento 		reconocimiento;
	struct mensaje_nombre_request 		nombre_request;
	struct mensaje_nombre_reply 		nombre_reply;
	struct mensaje_desconexion			desconexion;
	mensaje_t 							tipo_mensaje;
	vector<clienteid_t>					clientes;
	uint8_t								buffer[200];
	ssize_t 							mensaje_size;

	UNUSED(mensaje_size);

	// Este puntero se rellenará con eventos en la llamada a epoll_wait. Antes de llamarlo
	// hay que alojar memoria para almacenar hasta MAXEVENTS eventos.
	events = (epoll_event*) calloc(MAXEVENTS, sizeof(events));

	do {
		/* epoll_wait devuelve cuantos descriptores hay disponibles para leer y el puntero de
		eventos lo rellena con un vector de eventos con los descriptores y las flags asociadas
		al evento */
		desc_ready = epoll_wait(epoll_thread_fd, events, MAXEVENTS, -1);

		/* Si epoll tiene un error, devolverá negativo */
		if(desc_ready < 0)
		{
			perror("[GRUPO_HILO] epoll_wait() error");
		}

		/* Si no da error, epoll devuelve el número de descriptores que han disparado los eventos; es decir
		los que están preparados con datos para leer */
		for(int i = 0; i < desc_ready; i++)
		{
			/* Por cada descriptor disponible tenemos una estructura epoll_event lista.
			Lo único que tenemos que hacer es iterar sobre este vector y obtener el
			descriptor de socket asociado a cada evento */
			socket = events[i].data.fd;

			// Leemos los 8 primeros bits, correspondientes al tipo de mensaje
			rc = recv(socket, &tipo_mensaje, sizeof(uint8_t),0);

			/* Tanto en caso de que haya un error leyendo dicho socket, o de que la lectura devuelva cero,
			se procederá a eliminar al cliente del grupo y cerrar la conexión */
			if(rc <= 0)
			{
				// Mostramos mensaje por consola
				printf("[DESCONEXIÓN] Socket: %d, recv:%d\n", socket, rc);

				// Eliminamos el socket para que el hilo ya no compruebe sus mensajes y cerramos conexión
				epoll_ctl(epoll_thread_fd, EPOLL_CTL_DEL, socket, NULL);
				close(socket);

				/* Además, puesto que es el servidor el único que es consciente de estas desconexiones, creamos
				un mensaje de desconexión, indicando quién se ha desconectado, y se lo enviamos a los demás
				miembros del grupo */

				// El primer byte siempre es el tipo de mensaje
				buffer[0] = MENSAJE_DESCONEXION;

				// La ID Origen se refiere al cliente que se ha desconectado
				desconexion.cliente_id_origen = socket;

				// Copiamos la estructura del mensaje a contuniación del byte de tipo de mensaje
				memcpy(&buffer[1],&desconexion, sizeof(desconexion));

				/* Ahora, recorreremos toda la lista de clientes conocidos para eliminar el que se acaba de 
				desconectar */
				for(uint j=0; j < clientes.size(); j++) 
				{
					if(clientes[j]==socket)
					{
						// Antes de reenviar, debemos borrar el cliente desconectado de la lista
						cout << "Cliente ID: " << socket << " borrada." << endl;
						clientes.erase(clientes.begin() + j);
						break;
					}
				}
				for(uint j=0; j < clientes.size(); j++) 
				{
					/* Por cada miembro del grupo, eviamos el byte de tipo de mensaje y el mensaje.
					En clientes[j] se almacena el socket de cada cliente, le enviamos el buffer y un tamaño
					igual a el byte de tipo más la estructura mandada */
					do
					{
						rc = send(clientes[j], buffer, sizeof(mensaje_t) + sizeof(desconexion), 0);
						if (rc < 0)
						{
							perror("[DESCONEXIÓN] send() error ");
							printf("Socket conflictivo: %d\n", clientes[j]);
						}
					} while(rc < 0);

				}
				
				/* Si hemos recibido un mensaje de error al leer el mensaje o el cliente se ha desconectado, miramos
				el siguiente evento de epoll y no hace falta que miremos el tipo de mensaje ya que no hemos leído nada */
				continue;

			} else {

				/* En caso de recibir datos correctamente, miramos qué valor contiene ese primer byte leído, correspondiente
				al tipo de mensaje, y actuamos según sea necesário */
				switch (tipo_mensaje)
				{
				case MENSAJE_SALUDO:
					
					/* Los mensaje de saludo son obligatorios para todos los clientes. Cuando el servidor les consiga unir a
					un grupo, estos deberán mandar un único mensaje de saludo indicando su nombre y su id. */

					// Recibimos el resto del mensaje, la estructura mensaje_saludo
					rc = recv(socket, &saludo, sizeof(saludo), 0);
					if(rc < 0)
					{
						perror("recv() error");
						break;
					}
					
					// Copiamos el tipo de mensaje al primer byte del buffer
					memcpy(&buffer[0], &tipo_mensaje, sizeof(tipo_mensaje));

					// Y el mismo mensaje de saludo a partir del segundo byte
					memcpy(&buffer[1], &saludo, sizeof(saludo));

					/* Enviamos el mensaje de saludo a cada cliente conocido. Este reenvío no es estrictamente necesário
					ya que los propios clientes disponen de mecanismos para averiguar la identidad de miembros del grupo
					de los cuales no han recibido su saludo. Aún así, este sencillo mensaje de saludo ayuda a no saturar la
					red cada vez que se conecta un nuevo cliente */
					for(uint j=0; j < clientes.size(); j++) 
					{
						rc = send(clientes[j],buffer, sizeof(saludo) + sizeof(tipo_mensaje), 0);

						if(rc < 0)
						{
							perror("[SALUDO] send() error");
							break;
						}
					}

					/* Al terminar, añadimos el cliente a nuestra lista. Importante remarcar que no hace falta que añadamos
					el socket a nuestro identificador de epoll, pues esa tarea se delega en el hilo principal que maneja la
					conexión de nuevos clientes */
					clientes.push_back(socket);
					break;

				case MENSAJE_POSICION:
					/* Los mensajes de posición no implican ninguna lógica en el lado del servidor y no hay que procesar
					nada, siendo la única tarea del servidor la de reenviar el paquete a los demás vecinos */

					// Recibimos el resto del mensaje y comprobamos errores
					rc = recv(socket, &posicion, sizeof(posicion), 0);
					if(rc < 0)
					{
						perror("recv() error");
						break;
					}

					// Copiamos el tipo de mensaje al primer byte del buffer
					memcpy(&buffer[0], &tipo_mensaje, sizeof(tipo_mensaje));

					// Copiamos la misma estructura de posición que hemos leído a partir del segundo byte del buffer
					memcpy(&buffer[1], &posicion, sizeof(posicion));

					/* Por cada cliente conocido, reenviamos la información */
					for(uint j=0; j < clientes.size(); j++) 
					{
						if(clientes[j]!=socket)
						{
							rc = send(clientes[j],buffer, sizeof(tipo_mensaje) + sizeof(posicion), 0);

							if(rc < 0)
							{
								perror("[POSICIÓN] send() error");
								break;
							}
						}
					}
					break;

				case MENSAJE_RECONOCIMIENTO:

					/* El mensaje de reconocimiento no es una inundación indiscriminada como el saludo o la posición,
					sino que hay que reenviar el paquete sólo al cliente destino. Gracias a que las ID son las mismas que
					los descriptores usadas por el servidor, tan sólo hay que reenviar el paquete al socket que aparezca
					en el campo de ID destino. */

					// Recibimos la estructura y comprobamos errores
					rc = recv(socket, &reconocimiento, sizeof(reconocimiento), 0);
					if(rc < 0)
					{
						perror("recv() error");
						break;
					}

					// Copiamos el tipo de mensaje leído al primer byte del buffer
					memcpy(&buffer[0], &tipo_mensaje, sizeof(tipo_mensaje));

					// Copiamos la misma estructura de mensaje a partir del segundo byte del buffer
					memcpy(&buffer[1], &reconocimiento, sizeof(reconocimiento));

					// Y simplemente enviamos el paquete de vuelta al campo destino en caso de ser un cliente real
					//cout << "Recibido ACK de " << socket << " hacia " << reconocimiento.cliente_id_destino << endl;

					rc = send(reconocimiento.cliente_id_destino, buffer, sizeof(tipo_mensaje) + sizeof(reconocimiento), 0);

					if(rc < 0)
					{
						//perror("[RECONOCIMIENTO] send() error");
						break;
					}
					break;

				case MENSAJE_NOMBRE_REQUEST:

					/* Los mensajes de petición de nombre se usan cuando un cliente desconoce la identidad de algún vecino
					y le envía un mensaje para descubrir su nombre y poder mostrarlo en la interfaz y esperar sus ACKs (Lo
					que llamamos por 'conocerlo'). Al igual que los mensajes de reconocimiento, estos mensajes tienen un
					destinatario, y la función del servidor será simplemente reenviarlo a esa ID */

					// Recibimos la estructura y comprobamos errores
					rc = recv(socket, &nombre_request, sizeof(nombre_request), 0);
					if(rc < 0)
					{
						perror("recv() error");
						break;
					}

					// Copiamos el tipo de mensaje leído al primer byte del buffer
					memcpy(&buffer[0], &tipo_mensaje, sizeof(tipo_mensaje));

					// Copiamos el mismo mensaje recibido a partir del segundo byte del buffer
					memcpy(&buffer[1], &nombre_request, sizeof(nombre_request));

					// Enviamos a su destinatario el mensaje completo con su identificador de tipo
					rc = send(nombre_request.cliente_id_destino, buffer, sizeof(tipo_mensaje) + sizeof(nombre_request), 0);

					if(rc < 0)
					{
						perror("[NOMBRE_REQUEST] send() error");
						break;
					}
					break;

				case MENSAJE_NOMBRE_REPLY:

					/* Los mensajes de respuesta de nombre son idénticos para el servidor. La única diferencia reside en
					que estos llevan además un campo de nombre, que usará el cliente para anotarse la identidad de su 
					vecino. Se actuará igual que en el caso de petición de nombre o de mensaje de reconocimiento */

					// Recibimos la estructura y comprobamos errores
					rc = recv(socket, &nombre_reply, sizeof(nombre_reply), 0);
					if(rc < 0)
					{
						perror("recv() error");
						break;
					}

					// Copiamos el tipo de mensaje leído al primer byte del buffer
					memcpy(&buffer[0], &tipo_mensaje, sizeof(tipo_mensaje));

					// Copiamos el mismo mensaje recibido a partir del segundo byte del buffer
					memcpy(&buffer[1], &nombre_reply, sizeof(nombre_reply));
					
					// Devolvemos la respuesta al destino 
					rc = send(nombre_reply.cliente_id_destino, buffer, sizeof(tipo_mensaje) + sizeof(nombre_reply), 0);

					if(rc < 0)
					{
						//perror("[NOMBRE_REPLY] send() error");
						break;
					}
					break;
				default:

					/* En caso de no reconocer el tipo de mensaje que nos ha llegado, mostramos un mensaje de error */
					printf("[ERROR] Mensaje no reconocido. Socket: %d. Mensaje: %02X\n", socket, tipo_mensaje);
				}
			}
		}
	} while (!cerrar_hilo);
}


/* Función principal del servidor. La función corre sobre el hilo principal, y no realiza ninguna
tarea de reenvío de paquetes. El hilo principal comprobará si hay conexiones entrantes, y esperará
hasta escuchar su mensaje de conexión a grupo, momento en que notificará al hilo del grupo que, a partir
de ahora, también escuche los mensajes de este nuevo cliente. Si no existe el grupo con la ID solicitada,
entonces se creará un nuevo hilo */
int main (int argc, char *argv[])
{
   int    listen_sd, epoll_fd;
   struct epoll_event event;
   struct epoll_event epoll_events[MAXEVENTS];

   listen_sd = aio_socket_escucha(SERVER_PORT);
   epoll_fd = epoll_create1(0);

   event.data.fd = listen_sd;
   event.events = EPOLLIN;

   vector<clienteid_t> clientes_conocidos;

   //epoll_events = (epoll_event*) calloc (MAXEVENTS, sizeof(event));

   epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sd, &event);

   int epoll_n;

   /* A continuación empieza el bucle principal del servidor, que sólo terminará cuando haya ocurrido algún error.
   Este bucle corre solamente desde el hilo principal, y será desde este donde se realizará la conexión de nuevos
   clientes y se añadirán al set de su grupo correspondiente, o se creará un nuevo hilo en caso de no existir este */
   do
   {
   		epoll_n = epoll_wait(epoll_fd, epoll_events, MAXEVENTS, -1);

   		// Por cada evento que devuelva epoll, comprobamos que no es un error
		for (int i = 0; i < epoll_n; i++)
		{
		    if ((epoll_events[i].events & EPOLLERR) || (epoll_events[i].events & EPOLLHUP) || (!(epoll_events[i].events & EPOLLIN)))
		    {
		        perror("epoll_wait() error");
		        continue;
		    }

		    if(epoll_events[i].data.fd == listen_sd)
		    {
#ifdef _DEBUG_
		    	printf("Recibida nueva conexión.\n");
#endif
		    	int new_client_sd;

		    	do
			    {
			    	struct sockaddr_in new_client_sockaddr;
	    			socklen_t clientsize = sizeof(new_client_sockaddr);
			    	new_client_sd = accept4(listen_sd, (struct sockaddr *)&new_client_sockaddr, &clientsize, SOCK_NONBLOCK);
			    	if(new_client_sd < 0)
			    	{
			    		if(errno != EWOULDBLOCK || errno != EAGAIN)
			    		{
			    			perror("accept4()");
			    		}
			    		break;
			    	}

			    	epoll_event client_event;
			    	client_event.data.fd = new_client_sd;
			    	client_event.events = EPOLLIN;
#ifdef _DEBUG_
		    		cout << "Nuevo cliente en socket: " << new_client_sd << endl <<flush;
#endif

			    	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client_sd, &client_event) < 0)
			    	{
			    		perror("epoll_ctl()");
			    	}
			    } while (new_client_sd >= 0);

		    } else {
#ifdef _DEBUG_
		    	printf("Recibidos datos en Socket %d.\n", epoll_events[i].data.fd);
#endif
		    	struct mensaje_conexion nueva_conexion;
		    	int rc, socket = epoll_events[i].data.fd;
		    	mensaje_t tipo_mensaje;

		    	rc = read(socket, &tipo_mensaje, sizeof(mensaje_t));
		    	if(rc <= 0)
		    	{
		    		perror("Error al leer tipo de mensaje.");
		    		close(socket);
		    		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, socket, NULL);
		    		continue;
		    	}
		    	rc = read(socket, &nueva_conexion, sizeof(struct mensaje_conexion));
		    	grupoid_t grupo = nueva_conexion.grupo;

		    	//grupoid_t grupo = aio_lectura_grupo(epoll_events[i].data.fd);
		    	if(grupo < 0)
		    	{
#ifdef _DEBUG_
		    		perror("Error en recepción de mensaje de conexión.");
#endif
		    		close(socket);
		    		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, socket, NULL);
		    		continue;
		    	} else {
		    		clientes_conocidos.push_back(socket);
#ifdef _DEBUG_
		    		printf("Recibida petición a GrupoID: %d\n", grupo);
		    		printf("Conectados: %lu Clientes.\n", clientes_conocidos.size());
#endif
		    	}

		    }
		}
    } while (TRUE);


    close(listen_sd);

    return 0;
}
