/**
 * @file    app_sandbox_seccomp.c
 * @brief   seccomp 白名单规则（强化沙箱安全性）
 * @version LN-B-4.2.0.0
 * @fix     移除 gethostname/getdomainname，替换 seccomp_strerror
 */

#define _GNU_SOURCE
#include <seccomp.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "log_extra.h"

/* ============================================================
 * 白名单系统调用列表（只允许必要调用）
 * ============================================================ */

/* 基础文件操作 */
static const int allowed_syscalls_basic[] = {
    SCMP_SYS(read),           /* 读取 */
    SCMP_SYS(write),          /* 写入 */
    SCMP_SYS(open),           /* 打开文件（兼容旧） */
    SCMP_SYS(openat),         /* 打开文件 */
    SCMP_SYS(close),          /* 关闭文件 */
    SCMP_SYS(stat),           /* 文件状态 */
    SCMP_SYS(lstat),          /* 链接状态 */
    SCMP_SYS(fstat),          /* 文件描述符状态 */
    SCMP_SYS(newfstatat),     /* 相对路径状态 */
    SCMP_SYS(access),         /* 访问权限检查 */
    SCMP_SYS(faccessat),      /* 相对路径访问 */
    SCMP_SYS(readlink),       /* 读链接 */
    SCMP_SYS(readlinkat),     /* 相对路径读链接 */
    SCMP_SYS(getdents),       /* 读目录项 */
    SCMP_SYS(getdents64),     /* 读目录项（64位） */
    SCMP_SYS(lseek),          /* 文件偏移 */
    SCMP_SYS(dup),            /* 复制文件描述符 */
    SCMP_SYS(dup2),           /* 复制文件描述符（指定） */
    SCMP_SYS(dup3),           /* 复制文件描述符（带标志） */
    SCMP_SYS(fcntl),          /* 文件控制 */
    SCMP_SYS(flock),          /* 文件锁 */
    SCMP_SYS(fallocate),      /* 预分配空间 */
    SCMP_SYS(ftruncate),      /* 截断文件 */
    SCMP_SYS(truncate),       /* 截断文件（路径） */
    SCMP_SYS(pread64),        /* 预读 */
    SCMP_SYS(pwrite64),       /* 预写 */
};

/* 内存管理 */
static const int allowed_syscalls_memory[] = {
    SCMP_SYS(brk),            /* 堆管理 */
    SCMP_SYS(mmap),           /* 内存映射 */
    SCMP_SYS(munmap),         /* 取消映射 */
    SCMP_SYS(mprotect),       /* 内存保护 */
    SCMP_SYS(madvise),        /* 内存建议 */
    SCMP_SYS(mlock),          /* 锁定内存 */
    SCMP_SYS(munlock),        /* 解锁内存 */
    SCMP_SYS(mlockall),       /* 锁定所有内存 */
    SCMP_SYS(munlockall),     /* 解锁所有内存 */
    SCMP_SYS(mincore),        /* 内存驻留状态 */
};

/* 进程管理 */
static const int allowed_syscalls_process[] = {
    SCMP_SYS(clone),          /* 创建进程（受限） */
    SCMP_SYS(fork),           /* 创建进程 */
    SCMP_SYS(vfork),          /* 创建进程（变体） */
    SCMP_SYS(execve),         /* 执行程序 */
    SCMP_SYS(execveat),       /* 执行程序（相对路径） */
    SCMP_SYS(exit),           /* 退出 */
    SCMP_SYS(exit_group),     /* 退出组 */
    SCMP_SYS(wait4),          /* 等待进程 */
    SCMP_SYS(waitpid),        /* 等待进程ID */
    SCMP_SYS(kill),           /* 发送信号 */
    SCMP_SYS(tkill),          /* 发送信号（线程） */
    SCMP_SYS(tgkill),         /* 发送信号（线程组） */
    SCMP_SYS(getpid),         /* 获取进程ID */
    SCMP_SYS(getppid),        /* 获取父进程ID */
    SCMP_SYS(gettid),         /* 获取线程ID */
    SCMP_SYS(sched_yield),    /* 调度让步 */
    SCMP_SYS(nanosleep),      /* 纳秒睡眠 */
    SCMP_SYS(clock_nanosleep),/* 时钟纳秒睡眠 */
};

/* 信号处理 */
static const int allowed_syscalls_signal[] = {
    SCMP_SYS(sigaction),      /* 信号处理 */
    SCMP_SYS(signal),         /* 信号处理（旧） */
    SCMP_SYS(sigreturn),      /* 信号返回 */
    SCMP_SYS(rt_sigaction),   /* 实时信号处理 */
    SCMP_SYS(rt_sigprocmask), /* 实时信号掩码 */
    SCMP_SYS(rt_sigreturn),   /* 实时信号返回 */
    SCMP_SYS(rt_sigpending),  /* 实时待处理信号 */
    SCMP_SYS(rt_sigtimedwait),/* 实时信号等待 */
    SCMP_SYS(sigaltstack),    /* 信号栈 */
};

/* 定时器 */
static const int allowed_syscalls_timer[] = {
    SCMP_SYS(gettimeofday),   /* 获取时间 */
    SCMP_SYS(time),           /* 获取时间 */
    SCMP_SYS(clock_gettime),  /* 时钟获取 */
    SCMP_SYS(clock_getres),   /* 时钟分辨率 */
    SCMP_SYS(clock_settime),  /* 时钟设置（受限） */
    SCMP_SYS(timer_create),   /* 创建定时器 */
    SCMP_SYS(timer_settime),  /* 设置定时器 */
    SCMP_SYS(timer_gettime),  /* 获取定时器 */
    SCMP_SYS(timer_delete),   /* 删除定时器 */
};

/* 网络（基础） */
static const int allowed_syscalls_network[] = {
    SCMP_SYS(socket),         /* 创建套接字 */
    SCMP_SYS(connect),        /* 连接 */
    SCMP_SYS(accept),         /* 接受连接 */
    SCMP_SYS(accept4),        /* 接受连接（带标志） */
    SCMP_SYS(bind),           /* 绑定地址 */
    SCMP_SYS(listen),         /* 监听 */
    SCMP_SYS(getsockname),    /* 获取套接字名称 */
    SCMP_SYS(getpeername),    /* 获取对端名称 */
    SCMP_SYS(setsockopt),     /* 设置套接字选项 */
    SCMP_SYS(getsockopt),     /* 获取套接字选项 */
    SCMP_SYS(sendto),         /* 发送到 */
    SCMP_SYS(sendmsg),        /* 发送消息 */
    SCMP_SYS(recvfrom),       /* 接收自 */
    SCMP_SYS(recvmsg),        /* 接收消息 */
    SCMP_SYS(shutdown),       /* 关闭 */
    SCMP_SYS(poll),           /* 轮询 */
    SCMP_SYS(select),         /* 选择 */
    SCMP_SYS(epoll_create),   /* 创建 epoll */
    SCMP_SYS(epoll_ctl),      /* 控制 epoll */
    SCMP_SYS(epoll_wait),     /* 等待 epoll */
    SCMP_SYS(epoll_pwait),    /* 等待 epoll（带信号） */
    SCMP_SYS(pipe),           /* 创建管道 */
    SCMP_SYS(pipe2),          /* 创建管道（带标志） */
    SCMP_SYS(eventfd),        /* 事件文件描述符 */
    SCMP_SYS(eventfd2),       /* 事件文件描述符（带标志） */
    SCMP_SYS(signalfd),       /* 信号文件描述符 */
    SCMP_SYS(signalfd4),      /* 信号文件描述符（带标志） */
    SCMP_SYS(timerfd_create), /* 定时器文件描述符 */
    SCMP_SYS(timerfd_settime),/* 设置定时器 */
    SCMP_SYS(timerfd_gettime),/* 获取定时器 */
};

/* UID/GID（只读/基本） */
static const int allowed_syscalls_uid[] = {
    SCMP_SYS(getuid),         /* 获取用户ID */
    SCMP_SYS(geteuid),        /* 获取有效用户ID */
    SCMP_SYS(getgid),         /* 获取组ID */
    SCMP_SYS(getegid),        /* 获取有效组ID */
    SCMP_SYS(getgroups),      /* 获取组列表 */
    SCMP_SYS(setuid),         /* 设置用户ID（受限） */
    SCMP_SYS(setgid),         /* 设置组ID（受限） */
};

/* 系统信息（移除了 gethostname/getdomainname，避免宏缺失） */
static const int allowed_syscalls_sysinfo[] = {
    SCMP_SYS(uname),          /* 系统名称 */
    SCMP_SYS(sysinfo),        /* 系统信息 */
    /* 不再添加 gethostname 和 getdomainname（非关键，且某些架构未定义） */
};

/* ============================================================
 * 内部辅助：添加系统调用到白名单
 * ============================================================ */

static int add_syscall_list(scmp_filter_ctx ctx, const int *list, size_t count) {
    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, list[i], 0) != 0) {
            LOG_WARN_T("Seccomp", "AddList", "Fail", "syscall %d add failed", list[i]);
            ret = -1;
        }
    }
    return ret;
}

/* ============================================================
 * 公共 API：应用 seccomp 规则
 * ============================================================ */

int app_sandbox_apply_seccomp(void) {
    LOG_INFO_T("Seccomp", "Apply", "Enter", "applying seccomp whitelist rules");

    /* 初始化 seccomp 上下文（默认拒绝所有） */
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);
    if (!ctx) {
        LOG_ERROR_T("Seccomp", "Apply", "InitFail", "seccomp_init failed");
        return -1;
    }

    int ret = 0;

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding basic file syscalls (%zu)",
                sizeof(allowed_syscalls_basic) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_basic,
                         sizeof(allowed_syscalls_basic) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding memory syscalls (%zu)",
                sizeof(allowed_syscalls_memory) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_memory,
                         sizeof(allowed_syscalls_memory) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding process syscalls (%zu)",
                sizeof(allowed_syscalls_process) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_process,
                         sizeof(allowed_syscalls_process) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding signal syscalls (%zu)",
                sizeof(allowed_syscalls_signal) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_signal,
                         sizeof(allowed_syscalls_signal) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding timer syscalls (%zu)",
                sizeof(allowed_syscalls_timer) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_timer,
                         sizeof(allowed_syscalls_timer) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding network syscalls (%zu)",
                sizeof(allowed_syscalls_network) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_network,
                         sizeof(allowed_syscalls_network) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding uid syscalls (%zu)",
                sizeof(allowed_syscalls_uid) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_uid,
                         sizeof(allowed_syscalls_uid) / sizeof(int)) != 0) {
        ret = -1;
    }

    LOG_DEBUG_T("Seccomp", "Apply", "Add", "adding sysinfo syscalls (%zu)",
                sizeof(allowed_syscalls_sysinfo) / sizeof(int));
    if (add_syscall_list(ctx, allowed_syscalls_sysinfo,
                         sizeof(allowed_syscalls_sysinfo) / sizeof(int)) != 0) {
        ret = -1;
    }

    /* 加载规则 */
    if (seccomp_load(ctx) != 0) {
        LOG_ERROR_T("Seccomp", "Apply", "LoadFail", "seccomp_load failed");
        seccomp_release(ctx);
        return -1;
    }

    seccomp_release(ctx);

    if (ret == 0) {
        LOG_INFO_T("Seccomp", "Apply", "OK", "seccomp whitelist applied successfully");
    } else {
        LOG_WARN_T("Seccomp", "Apply", "Partial", "some syscalls failed to add, but loaded");
    }

    return ret;
}