// exs_err_log.exs : there is no `log`; world text is `tell`, author text is `trace`
type
    class C
        public
            verb main(player is obj) returns int
                log("hi")
                return 0
            endverb
    endclass
