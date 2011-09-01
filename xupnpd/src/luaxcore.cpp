#include "luaxcore.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "mem.h"
#include <time.h>
#include "mcast.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* TODO:
- fork for async io
- HTTP client
- child to parent events over SOCK_DGRAM socket
*/

namespace core
{
    struct timer_event
    {
        time_t tv;
        const char* name;
        int sec;
        timer_event* next;
    };


    timer_event* timers=0;

    struct listener
    {
        int port;
        int fd;
        const char* name;
        listener* next;
    };

    listener* listeners=0,*listeners_end=0;

    int detached=0;             // daemon
    FILE* http_client_fp=0;     // for HTTP workers only

    mcast::mcast_grp ssdp_mcast_grp;
    int ssdp_upstream=-1;
    int ssdp_downstream=-1;

    int listener_add(const char* host,int port,const char* name,int backlog)
    {
        if(!name || !*name || port<1)
            return -1;

        if(!host || !*host)
        {
            if(*ssdp_mcast_grp.interface)
                host=ssdp_mcast_grp.interface;
            else
                host="*";
        }

        if(backlog<1)
            backlog=5;

        sockaddr_in sin;
        sin.sin_family=AF_INET;
        sin.sin_addr.s_addr=INADDR_ANY;
        sin.sin_port=htons(port);

        if(strcmp(host,"*") && strcmp(host,"any"))
            sin.sin_addr.s_addr=inet_addr(host);

        int fd=socket(sin.sin_family,SOCK_STREAM,0);
        if(fd==-1)
            return -2;

        int reuse=1;
        setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

        if(bind(fd,(sockaddr*)&sin,sizeof(sin)) || listen(fd,backlog))
        {
            close(fd);
            return -3;
        }

        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);

        int n=strlen(name);

        listener* l=(listener*)MALLOC(sizeof(listener)+n+1);

        l->port=port;
        l->fd=fd;
        l->name=(char*)(l+1);
        l->next=0;
        strcpy((char*)l->name,name);

        if(!listeners)
            listeners=listeners_end=l;
        else
        {
            listeners_end->next=l;
            listeners_end=l;
        }        

        return 0;
    }

    void listener_clear(void)
    {
        while(listeners)
        {
            listener* tmp=listeners;
            listeners=listeners->next;

            if(tmp->fd!=-1)
                close(tmp->fd);
            FREE(tmp);
        }

        listeners=listeners_end=0;
    }

    volatile int __sig_quit=0;
    volatile int __sig_alarm=0;
    volatile int __sig_child=0;
    volatile int __sig_usr1=0;
    volatile int __sig_usr2=0;

    int __sig_pipe[2]={-1,-1};

    char* parse_command_line(const char* cmd,char** dst,int n);

    void sig_handler(int n)
    {
        int e=errno;

        switch(n)
        {
        case SIGINT:
        case SIGQUIT:
        case SIGTERM:
            __sig_quit=1;
            break;
        case SIGALRM:
            __sig_alarm=1;
            break;
        case SIGCHLD:
            __sig_child=1;
            break;
        case SIGUSR1:
            __sig_usr1=1;
            break;
        case SIGUSR2:
            __sig_usr2=1;
            break;
        }

        send(__sig_pipe[1],"*",1,MSG_DONTWAIT);

        errno=e;
    }

    void timer_add(int sec,const char* name)
    {
        if(!name || !*name)
            return;

        timer_event* e=(timer_event*)MALLOC(sizeof(timer_event)+strlen(name)+1);
        e->next=0;
        e->tv=time(0)+sec;
        e->name=(char*)(e+1);
        strcpy((char*)e->name,name);
        e->sec=sec;
        if(!timers || e->tv<=timers->tv)
        {
            e->next=timers;
            timers=e;
        }else
        {
            for(timer_event* tmp=timers;tmp;tmp=tmp->next)
            {
                if(!tmp->next)
                {
                    tmp->next=e;
                    break;
                }else
                {
                    if(tmp->next->tv>=e->tv)
                    {
                        e->next=tmp->next;
                        tmp->next=e;
                        break;
                    }
                }                
            }
        }
    }

    void timer_reset(void)
    {
        int sec=0;

        if(timers)
        {
            sec=timers->tv-time(0);
            if(sec<1)
                sec=1;
        }

        alarm(sec);
    }

    void timer_clear(void)
    {
        alarm(0);

        while(timers)
        {
            timer_event* tmp=timers;
            timers=timers->next;
            FREE(tmp);
        }
        timers=0;
    }

    void ssdp_done(void)
    {
        if(ssdp_upstream!=-1)
        {
            ssdp_mcast_grp.close(ssdp_upstream);
            ssdp_upstream=-1;
        }

        if(core::ssdp_downstream!=-1)
        {
            ssdp_mcast_grp.leave(ssdp_downstream);
            ssdp_downstream=-1;
        }
    }

    void ssdp_clear(void)
    {
        if(ssdp_upstream!=-1)
        {
            ssdp_mcast_grp.close(ssdp_upstream);
            ssdp_upstream=-1;
        }

        if(core::ssdp_downstream!=-1)
        {
            ssdp_mcast_grp.close(ssdp_downstream);
            ssdp_downstream=-1;
        }
    }

    pid_t fork_process(int detach)
    {
        pid_t pid=fork();

        if(!pid)
        {
            alarm(0);

            for(int i=0;i<sizeof(core::__sig_pipe)/sizeof(*core::__sig_pipe);i++)
                close(core::__sig_pipe[i]);

            core::ssdp_clear();
            core::listener_clear();

            if(detach)
            {
                int fd=open("/dev/null",O_RDWR);
                if(fd>=0)
                {
                    for(int i=0;i<3;i++)
                        dup2(fd,i);
                    close(fd);
                }else
                    for(int i=0;i<3;i++)
                        close(i);
            }
        }

        return pid;
    }

    void process_event(lua_State* L,const char* name,int arg1)
    {
        lua_getglobal(L,"events");

        lua_getfield(L,-1,name);

        if(lua_type(L,-1)==LUA_TFUNCTION)
        {
            lua_pushstring(L,name);
            lua_pushinteger(L,arg1);
            if(lua_pcall(L,2,0,0))
            {
                if(!detached)
                    fprintf(stderr,"%s\n",lua_tostring(L,-1));
                else
                    syslog(LOG_INFO,"%s",lua_tostring(L,-1));
                lua_pop(L,1);
            }
        }else
            lua_pop(L,1);

        lua_pop(L,1);
    }

    void process_signals(lua_State* L)
    {
//printf("%i\n",lua_gettop(L));

        char buf[128];
        while(recv(__sig_pipe[0],buf,sizeof(buf),MSG_DONTWAIT)>0);

        if(__sig_usr1)
            { __sig_usr1=0; process_event(L,"SIGUSR1",0); }
        if(__sig_usr2)
            { __sig_usr2=0; process_event(L,"SIGUSR2",0); }

        if(__sig_child)
        {
            __sig_child=0;

            pid_t pid;
            int status=0;

            lua_getglobal(L,"childs");

            while((pid=wait3(&status,WNOHANG,0))>0)
            {
                int del=0;

                lua_pushinteger(L,pid);
                lua_gettable(L,-2);

                if(lua_type(L,-1)==LUA_TTABLE)
                {
                    del=1;

                    lua_getfield(L,-1,"event");

                    const char* event=lua_tostring(L,-1);
                    if(event)
                    {
                        if(WIFEXITED(status))
                            status=WEXITSTATUS(status);
                        else
                            status=128;
                                                
                        process_event(L,event,status);
                    }
                    lua_pop(L,1);
                }else
                {
                    // not found, internal?
                }

                lua_pop(L,1);

                if(del)
                {
                    lua_pushinteger(L,pid);
                    lua_pushnil(L);
                    lua_rawset(L,-3);    
                }
            }

            lua_pop(L,1);
        }        

        if(__sig_alarm)
        {
            __sig_alarm=0;

            time_t t=time(0);

            while(core::timers && core::timers->tv<=t)
            {
                core::timer_event* tmp=core::timers;

                process_event(L,core::timers->name,core::timers->sec);

                core::timers=core::timers->next;

                FREE(tmp);
            }

            core::timer_reset();
        }

//printf("%i\n",lua_gettop(L));
    }


    void add_http_hdr_to_table(lua_State* L,char* p1,int idx)
    {
        if(!idx)
        {
            lua_pushstring(L,"reqline");
            lua_newtable(L);

            int nn=1;
            for(char* pp1=p1,*pp2;pp1;pp1=pp2)
            {
                pp2=strchr(pp1,' ');
                if(pp2)
                {
                    *pp2=0;
                    pp2++;
                    while(*pp2 && *pp2==' ')
                        pp2++;
                }
                if(*pp1)
                {
                    lua_pushinteger(L,nn++);
                    lua_pushstring(L,pp1);
                    lua_rawset(L,-3);
                }                                
            }
            lua_rawset(L,-3);
        }else
        {
            char* p3=strchr(p1,':');
            if(p3)
            {
                *p3=0;
                p3++;
                while(*p3 && *p3==' ')
                    p3++;

                if(*p3=='\"')
                {
                    p3++;
                    char* p=strchr(p3,'\"');
                    if(p)
                        *p=0;
                }

                if(*p1 && *p3)
                {
                    if(!strcasecmp(p1,"Content-Length"))
                    {
                        lua_pushstring(L,"length");
                        lua_pushstring(L,p3);
                        lua_rawset(L,-3);
                    }

                    lua_pushstring(L,p1);
                    lua_pushstring(L,p3);
                    lua_rawset(L,-3);
                }
            }
        }
    }

    void process_ssdp(lua_State* L)
    {
//printf("%i\n",lua_gettop(L));
        char buf[4096];
        int nbuf=0;

        char from[64]="";

        lua_getglobal(L,"events");
        
        while((nbuf=core::ssdp_mcast_grp.recv(ssdp_downstream,buf,sizeof(buf)-1,from,MSG_DONTWAIT))>0)
        {
            buf[nbuf]=0;

            static const char ssdp_tag[]="SSDP";

            lua_getfield(L,-1,ssdp_tag);

            if(lua_type(L,-1)==LUA_TFUNCTION)
            {
                lua_pushstring(L,ssdp_tag);
                lua_pushstring(L,from);

                lua_newtable(L);

                int idx=0;

                for(char* p1=buf,*p2;p1;p1=p2)
                {
                    while(*p1 && (*p1==' ' || *p1=='\r' || *p1=='\n' || *p1=='\t'))
                        p1++;

                    p2=strpbrk(p1,"\r\n");
                    if(p2)
                        { *p2=0; p2++; }

                    if(*p1)
                        add_http_hdr_to_table(L,p1,idx++);

                }

                if(lua_pcall(L,3,0,0))
                {
                    if(!detached)
                        fprintf(stderr,"%s\n",lua_tostring(L,-1));
                    else
                        syslog(LOG_INFO,"%s",lua_tostring(L,-1));
                    lua_pop(L,1);
                }
            }else
                lua_pop(L,1);


        }

        lua_pop(L,1);

//printf("%i\n",lua_gettop(L));

    }

    void process_http(lua_State* L,listener* l)
    {
        int fd;
        sockaddr_in sin;
        socklen_t sin_len=sizeof(sin);

        while((fd=accept(l->fd,(sockaddr*)&sin,&sin_len))>=0)
        {
            char name[64];
            int n=snprintf(name,sizeof(name),"%s",l->name);
            if(n<0 || n>=sizeof(name))
                name[sizeof(name)-1]=0;
            int port=l->port;

            char from[64]="";
            sprintf(from,"%s:%i",inet_ntoa(sin.sin_addr),ntohs(sin.sin_port));

            pid_t pid=fork_process(0);

            if(!pid)
            {
                signal(SIGHUP,SIG_IGN);
                signal(SIGPIPE,SIG_DFL);
                signal(SIGINT,SIG_DFL);
                signal(SIGQUIT,SIG_DFL);
                signal(SIGTERM,SIG_DFL);
                signal(SIGALRM,SIG_DFL);
                signal(SIGUSR1,SIG_DFL);
                signal(SIGUSR2,SIG_DFL);
                signal(SIGCHLD,SIG_DFL);

                sigset_t full_sig_set;
                sigfillset(&full_sig_set);
                sigprocmask(SIG_UNBLOCK,&full_sig_set,0);

                int on=1;
                setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&on,sizeof(on));

                FILE* fp=fdopen(fd,"a+");
                if(fp)
                {
//printf("%i\n",lua_gettop(L));
                    http_client_fp=fp;

                    lua_getglobal(L,"events");

                    lua_getfield(L,-1,name);

                    if(lua_type(L,-1)==LUA_TFUNCTION)
                    {
                        lua_pushstring(L,name);
                        lua_pushstring(L,from);
                        lua_pushinteger(L,port);

                        lua_newtable(L);

                        char tmp[512];

                        int idx=0;

                        alarm(15);                              // 15 seconds for read request from client

                        while(fgets(tmp,sizeof(tmp),fp))
                        {
                            char* p=strpbrk(tmp,"\r\n");
                            if(p)
                                *p=0;
                            if(!*tmp)
                                break;
                            add_http_hdr_to_table(L,tmp,idx++);
                        }

                        lua_getfield(L,-1,"length");
                        if(lua_type(L,-1)!=LUA_TNIL)
                        {
                            int len=lua_tointeger(L,-1);
                            lua_pop(L,1);

                            if(len>0)
                            {
                                luaL_Buffer B;
                                luaL_buffinit(L,&B);
                                
                                int n=0;
                                while((n=fread(tmp,1,sizeof(tmp)>len?len:sizeof(tmp),fp))>0 && len>0)
                                {
                                    luaL_addlstring(&B,tmp,n);
                                    len-=n;
                                }

                                lua_pushstring(L,"data");
                                luaL_pushresult(&B);
                                lua_rawset(L,-3);
                            }

                        }else
                            lua_pop(L,1);

                        alarm(0);                               // reset read timer

                        if(lua_pcall(L,4,0,0))
                        {
                            if(!detached)
                                fprintf(stderr,"%s\n",lua_tostring(L,-1));
                            else
                                syslog(LOG_INFO,"%s",lua_tostring(L,-1));
                            lua_pop(L,1);
                        }

                    }else
                        lua_pop(L,1);

                    lua_pop(L,1);

                    fclose(fp);
                }else
                    close(fd);

//printf("%i\n",lua_gettop(L));

                exit(0);
            }

            close(fd);
        }
    }


}

static int lua_core_openlog(lua_State* L)
{
    const char* s=lua_tostring(L,1);
    const char* p=lua_tostring(L,2);

    if(!s)
        s="???";
    if(!p)
        p="";
    
    int f=LOG_SYSLOG;
    if(!strncmp(p,"local",5))
    {
        if(p[5]>47 && p[5]<56 && !p[6])
            f=((16+(p[5]-48))<<3);
    }else if(!strcmp(p,"daemon"))
        f=LOG_DAEMON;

    openlog(s,LOG_PID,f);
                                                    
    return 0;
}

static int lua_core_log(lua_State* L)
{
    char ss[512];
    int n=0;

    int count=lua_gettop(L);

    lua_getglobal(L,"tostring");

    for(int i=1;i<=count;i++)
    {
        lua_pushvalue(L,-1);
        lua_pushvalue(L,i);
        lua_call(L,1,1);

        size_t l=0;
        const char* s=lua_tolstring(L,-1,&l);
        if(s)
        {
            int m=sizeof(ss)-n;
            if(l>=m)
                l=m-1;

            memcpy(ss+n,s,l);
            n+=l;

            if(i<count)
            {
                if(n<sizeof(ss)-1)
                    ss[n++]=' ';
            }

        }
        lua_pop(L,1);
    }

    ss[n]=0;

    if(core::detached)
        syslog(LOG_INFO,"%s",ss);
    else
        fprintf(stdout,"%s\n",ss);

    return 0;
}

static int lua_core_detach(lua_State* L)
{
    pid_t pid=fork();
    if(pid==-1)
        return luaL_error(L,"can't fork process");
    else if(pid>0)
        exit(0);

    int fd=open("/dev/null",O_RDWR);
    if(fd>=0)
    {
        for(int i=0;i<3;i++)
            dup2(fd,i);
        close(fd);
    }else
        for(int i=0;i<3;i++)
            close(i);

    core::detached=1;
    lua_register(L,"print",lua_core_log);
    
    return 0;
}



static int lua_core_touchpid(lua_State* L)
{
    const char* s=lua_tostring(L,1);
    if(!s)
        return 0;

    FILE* fp=fopen(s,"r");
    if(fp)
        return luaL_error(L,"pid file already is exist");

    fp=fopen(s,"w");
    if(fp)
    {
        fprintf(fp,"%i",getpid());
        fclose(fp);
    }

    return 0;
}

// free(rc)!
char* core::parse_command_line(const char* cmd,char** dst,int n)
{
    char* s=strdup(cmd);

    int st=0;
    char* tok=0;
    int i=0;

    for(char* p=s;*p && i<n-1;p++)
    {
        switch(st)
        {
        case 0:
            if(*p==' ') continue;
            tok=p;
            if(*p=='\'' || *p=='\"') st=1; else st=2;
            break;
        case 1:
            if(*p=='\'' || *p=='\"')
                { *p=0; st=0; dst[i++]=tok+1; tok=0; }
            break;
        case 2:
            if(*p==' ')
                { *p=0; st=0; dst[i++]=tok; tok=0; }
            break;
        }
    }
    if(tok && i<n-1)
        dst[i++]=tok;

    dst[i]=0;

    return s;
}


static int lua_core_spawn(lua_State* L)
{
    const char* cmd=lua_tostring(L,1);
    const char* event=lua_tostring(L,2);

    int rc=0;

    if(!cmd || !event)
    {
        lua_pushinteger(L,rc);
        return 1;
    }

    pid_t pid=core::fork_process(1);

    if(pid!=-1)
    {
        if(!pid)
        {
            char* argv[128];
            core::parse_command_line(cmd,argv,sizeof(argv)/sizeof(*argv));
            if(argv[0])
                execvp(argv[0],argv);
            exit(127);
        }else
        {
            rc=1;

            lua_getglobal(L,"childs");

            lua_pushinteger(L,pid);
            lua_newtable(L);

            lua_pushstring(L,"cmd");
            lua_pushstring(L,cmd);
            lua_rawset(L,-3);

            lua_pushstring(L,"event");
            lua_pushstring(L,event);
            lua_rawset(L,-3);

            lua_rawset(L,-3);

            lua_pop(L,1);
        }
    }

    lua_pushinteger(L,rc);

    return 1;
}

static int lua_core_timer(lua_State* L)
{
    int sec=lua_tointeger(L,1);
    const char* event=lua_tostring(L,2);

    int rc=0;

    if(!sec || !event)
    {
        lua_pushinteger(L,rc);
        return 1;
    }

    core::timer_add(sec,event);

    core::timer_reset();

    lua_pushinteger(L,rc);

    return 1;
}

static int lua_core_uuid(lua_State* L)
{
    char buf[64];
    mcast::uuid_gen(buf);

    lua_pushstring(L,buf);

    return 1;
}


static int lua_core_mainloop(lua_State* L)
{
    using namespace core;

    if(__sig_quit)
        return 0;

    setsid();

    if(socketpair(PF_LOCAL,SOCK_STREAM,0,__sig_pipe))
        return luaL_error(L,"socketpair fail, cna't create signal pipe");


    struct sigaction action;
    sigset_t full_sig_set;

    memset((char*)&action,0,sizeof(action));
    sigfillset(&action.sa_mask);
    action.sa_handler=sig_handler;
    sigfillset(&full_sig_set);

    signal(SIGHUP,SIG_IGN);
    signal(SIGPIPE,SIG_IGN);
    sigaction(SIGINT,&action,0);
    sigaction(SIGQUIT,&action,0);
    sigaction(SIGTERM,&action,0);
    sigaction(SIGALRM,&action,0);
    sigaction(SIGUSR1,&action,0);
    sigaction(SIGUSR2,&action,0);
    sigaction(SIGCHLD,&action,0);

    sigprocmask(SIG_BLOCK,&full_sig_set,0);

    while(!__sig_quit)
    {
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(__sig_pipe[0],&fdset);

        int nfd=__sig_pipe[0];

        if(ssdp_downstream!=-1)
        {
            FD_SET(ssdp_downstream,&fdset);
            nfd=nfd<ssdp_downstream?ssdp_downstream:nfd;
        }

        for(listener* ll=listeners;ll;ll=ll->next)
        {
            FD_SET(ll->fd,&fdset);
            nfd=nfd<ll->fd?ll->fd:nfd;
        }

        nfd++;

        sigprocmask(SIG_UNBLOCK,&full_sig_set,0);

        int rc=select(nfd,&fdset,0,0,0);

        sigprocmask(SIG_BLOCK,&full_sig_set,0);

        if(rc==-1)
        {
            if(errno==EINTR)
            {
                process_signals(L);
                continue;
            }else
                break;
        }else if(!rc)
            continue;

        if(FD_ISSET(__sig_pipe[0],&fdset))
            process_signals(L);

        if(ssdp_downstream!=-1 && FD_ISSET(ssdp_downstream,&fdset))
            process_ssdp(L);

        for(listener* ll=listeners;ll;ll=ll->next)
            if(FD_ISSET(ll->fd,&fdset))
                process_http(L,ll);
    }

    sigprocmask(SIG_UNBLOCK,&full_sig_set,0);

    lua_getglobal(L,"atexit");
    lua_pushnil(L);
    while(lua_next(L,-2))
    {
        if(lua_type(L,-1)==LUA_TFUNCTION)
        {
            if(lua_pcall(L,0,0,0))
            {
                if(!detached)
                    fprintf(stderr,"%s\n",lua_tostring(L,-1));
                else
                    syslog(LOG_INFO,"%s",lua_tostring(L,-1));
                lua_pop(L,1);
            }
        }else
            lua_pop(L,1);
    }
    lua_pop(L,1);

    ssdp_done();
    listener_clear();

    signal(SIGTERM,SIG_IGN);
    signal(SIGCHLD,SIG_IGN);
    kill(0,SIGTERM);
                        
    for(int i=0;i<sizeof(__sig_pipe)/sizeof(*__sig_pipe);i++)
        close(__sig_pipe[i]);

    timer_clear();

    return 0;
}

static int lua_ssdp_init(lua_State* L)
{
    const char* iface=lua_tostring(L,1);
    int ttl=lua_tointeger(L,2);
    int loop=lua_tointeger(L,3);
    int debug=lua_tointeger(L,4);

    if(!iface)
        iface="eth0";

    if(ttl<1)
        ttl=1;

    if(!core::detached && debug>0)
    {
        mcast::verb_fp=stderr;

        if(debug>1)
            mcast::debug=1;
    }

    core::ssdp_done();

    int rc=0;

    if(!core::ssdp_mcast_grp.init("239.255.255.250:1900",iface,ttl,loop))
    {
        core::ssdp_upstream=core::ssdp_mcast_grp.upstream();
        if(core::ssdp_upstream!=-1)
        {
            core::ssdp_downstream=core::ssdp_mcast_grp.join();
            if(core::ssdp_downstream!=-1)
                rc=1;
            else
                core::ssdp_done();
        }
    }

    lua_pushinteger(L,rc);    

    return 1;
}

static int lua_ssdp_send(lua_State* L)
{
    size_t l=0;
    const char* s=lua_tolstring(L,1,&l);

    const char* to=lua_tostring(L,2);

    if(core::ssdp_upstream==-1 || !s || l<1)
        return 0;

    core::ssdp_mcast_grp.send(core::ssdp_upstream,s,l,to);

    return 0;
}

static int lua_ssdp_interface(lua_State* L)
{
    if(core::ssdp_downstream!=-1)
    {
        lua_pushstring(L,core::ssdp_mcast_grp.interface);
        return 1;
    }
    return 0;
}

static int lua_http_listen(lua_State* L)
{
    int port=lua_tointeger(L,1);
    const char* name=lua_tostring(L,2);
    const char* host=lua_tostring(L,3);
    int backlog=lua_tointeger(L,4);

    int rc=core::listener_add(host,port,name,backlog);
    
    lua_pushinteger(L,rc?0:1);

    return 1;
}

static int lua_http_send(lua_State* L)
{
    size_t l=0;
    const char* s=lua_tolstring(L,1,&l);

    if(core::http_client_fp)
        fwrite(s,1,l,core::http_client_fp);

    return 0;
}

static int lua_http_flush(lua_State* L)
{
    if(core::http_client_fp)
        fflush(core::http_client_fp);

    return 0;
}


int luaopen_luaxcore(lua_State* L)
{
    mcast::uuid_init();

    static const luaL_Reg lib_core[]=
    {
        {"detach",lua_core_detach},
        {"openlog",lua_core_openlog},
        {"log",lua_core_log},
        {"touchpid",lua_core_touchpid},
        {"spawn",lua_core_spawn},
        {"timer",lua_core_timer},
        {"uuid",lua_core_uuid},
        {"mainloop",lua_core_mainloop},
        {0,0}
    };

    static const luaL_Reg lib_ssdp[]=
    {
        {"init",lua_ssdp_init},
        {"send",lua_ssdp_send},
        {"interface",lua_ssdp_interface},
        {0,0}
    };

    static const luaL_Reg lib_http[]=
    {
        {"listen",lua_http_listen},
        {"send",lua_http_send},
        {"flush",lua_http_flush},
        {0,0}
    };

    luaL_register(L,"core",lib_core);
    luaL_register(L,"ssdp",lib_ssdp);
    luaL_register(L,"http",lib_http);

    lua_newtable(L);
    lua_setglobal(L,"events");

    lua_newtable(L);
    lua_setglobal(L,"childs");

    lua_newtable(L);
    lua_setglobal(L,"atexit");

    return 0;
}
