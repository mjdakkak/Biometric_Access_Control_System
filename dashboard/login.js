const loginForm = document.getElementById("login-form");
const loginResult = document.getElementById("login-result");
loginForm.addEventListener("submit", async function(event) {
    event.preventDefault();
    const username = document.getElementById("username").value.trim();
    const password = document.getElementById("password").value;
    const response = await fetch(
        "http://127.0.0.1:8000/admin/login",
        {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                username: username,
                password: password
            })
        }
    );
    const data = await response.json();
    if (data.success) {
        localStorage.setItem("admin_token", data.token);
        window.location.href = "index.html";
    } else {
        loginResult.innerHTML = `
            <p>Login failed: ${data.reason}</p>
        `;
    }
});