-- Rewrite intra-manual Markdown links to their rendered HTML form.
--
-- The manual mirrors the doc/ tree without the leading "doc/" component, so a
-- repo-root-relative link such as "doc/internals/ir.md" becomes a site link
-- "<prefix>internals/ir.html", where <prefix> is the "../" path from the
-- current page up to the site root (passed in SITE_PREFIX).  A bare sibling
-- link such as "ir.md" just changes extension.  External links (a scheme, or a
-- bare anchor) and links outside the manual are left untouched.

local prefix = os.getenv("SITE_PREFIX") or ""

function Link(el)
    local target = el.target
    if target:match("^%a[%w+.-]*://") or target:match("^#") or
       target:match("^mailto:") then
        return nil
    end
    local path, anchor = target:match("^([^#]*)(#?.*)$")
    if not path:match("%.md$") then
        return nil
    end
    path = path:gsub("^%./", "")
    if path:match("^doc/") then
        -- repo-root-relative into the manual: drop "doc/", anchor to site root
        path = prefix .. path:sub(5)
    elseif path:match("^%.%./") then
        -- points outside the manual (e.g. internal notes): leave as Markdown
        return nil
    end
    path = path:gsub("%.md$", ".html")
    el.target = path .. anchor
    return el
end
